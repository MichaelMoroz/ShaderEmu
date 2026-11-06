// Dear ImGui's platform and renderer for this machine (docs/apps/imgui.md).
#include "imgui_impl_shaderemu.h"
#include "imgui_internal.h"     // (ImDrawListSharedData: "Drawn later")
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "font_baked.h"
#include "imgui_format.h"
extern "C" {
#include <GLES/segl.h>
#include <nano-X.h>
}
#include "mcw.h"

static GR_WINDOW_ID window;
static GLuint font_texture;
static struct timeval last_frame;

// ---- text from numbers ----

// Dear ImGui's two format functions, which imgui.cpp is built without (build.sh): the common
// formats are written here (imgui_format.h) and the C library does the rest.
int ImFormatStringV(char* buf, size_t buf_size, const char* fmt, va_list args)
{
    int w = -1;
    if (buf && buf_size) {
        va_list same;
        va_copy(same, args);
        w = format_quick(buf, buf_size, fmt, same);
        va_end(same);
    }
    if (w < 0) w = vsnprintf(buf, buf_size, fmt, args);
    if (buf == NULL) return w;
    if (w == -1 || w >= (int)buf_size) w = (int)buf_size - 1;
    buf[w] = 0;
    return w;
}

int ImFormatString(char* buf, size_t buf_size, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int w = ImFormatStringV(buf, buf_size, fmt, args);
    va_end(args);
    return w;
}

// ---- input ----

// The machine's own words (docs/machine/input.md): from 0x20 the pointer's place, its buttons and the
// count of keys, and at 0x34 the clock in milliseconds.
static const volatile unsigned* control(int offset) { return (const volatile unsigned*)((const char*)seglPalette() - 0x400 + offset); }
static unsigned seen[4], asked_at;
static int asks = 8;

static ImGuiKey key_of(unsigned ch, bool ctrl)
{
    switch (ch) {
        case MWKEY_TAB: return ImGuiKey_Tab;
        case MWKEY_LEFT: return ImGuiKey_LeftArrow;
        case MWKEY_RIGHT: return ImGuiKey_RightArrow;
        case MWKEY_UP: return ImGuiKey_UpArrow;
        case MWKEY_DOWN: return ImGuiKey_DownArrow;
        case MWKEY_PAGEUP: return ImGuiKey_PageUp;
        case MWKEY_PAGEDOWN: return ImGuiKey_PageDown;
        case MWKEY_HOME: return ImGuiKey_Home;
        case MWKEY_END: return ImGuiKey_End;
        case MWKEY_INSERT: return ImGuiKey_Insert;
        case MWKEY_DELETE: return ImGuiKey_Delete;
        case MWKEY_BACKSPACE: return ImGuiKey_Backspace;
        case MWKEY_ENTER: return ImGuiKey_Enter;
        case MWKEY_ESCAPE: return ImGuiKey_Escape;
        case ' ': return ImGuiKey_Space;
        case MWKEY_LCTRL: return ImGuiKey_LeftCtrl;
        case MWKEY_RCTRL: return ImGuiKey_RightCtrl;
        case MWKEY_LSHIFT: return ImGuiKey_LeftShift;
        case MWKEY_RSHIFT: return ImGuiKey_RightShift;
        case MWKEY_LALT: return ImGuiKey_LeftAlt;
        case MWKEY_RALT: return ImGuiKey_RightAlt;
    }
    // the keyboard driver hands a letter with Ctrl over as its control character
    if (ctrl && ch >= 1 && ch <= 26) return (ImGuiKey)(ImGuiKey_A + ch - 1);
    if (ch >= 'a' && ch <= 'z') return (ImGuiKey)(ImGuiKey_A + ch - 'a');
    if (ch >= 'A' && ch <= 'Z') return (ImGuiKey)(ImGuiKey_A + ch - 'A');
    if (ch >= '0' && ch <= '9') return (ImGuiKey)(ImGuiKey_0 + ch - '0');
    return ImGuiKey_None;
}

static void key_event(const GR_EVENT_KEYSTROKE& k, bool down)
{
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = (k.modifiers & MWKMOD_CTRL) != 0;
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, (k.modifiers & MWKMOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (k.modifiers & MWKMOD_ALT) != 0);
    ImGuiKey key = key_of(k.ch, ctrl);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
    if (down && !ctrl && k.ch >= 32 && k.ch < 127) io.AddInputCharacter(k.ch);
}

// ---- the font ----

// Dear ImGui's default font as the build wrote it down (font_baked.h), put where the atlas
// would have built it. False when the program has fonts or settings of its own.
static bool baked_font(ImFontAtlas* atlas)
{
    if (atlas->ConfigData.Size || atlas->Fonts.Size || atlas->CustomRects.Size || atlas->Flags || atlas->TexDesiredWidth ||
        atlas->TexGlyphPadding != 1 || atlas->FontBuilderIO || atlas->FontBuilderFlags || strcmp(IMGUI_VERSION, BAKED_VERSION))
        return false;
    // the settings AddFontDefault() gives it; the font's file is not here, so nothing can build it again
    static const char no_file[4] = {0};
    const float* f = (const float*)baked_floats;
    ImFontConfig cfg;
    cfg.OversampleH = cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    cfg.SizePixels = 13.0f;
    strcpy(cfg.Name, "ProggyClean.ttf, 13px");
    cfg.EllipsisChar = (ImWchar)0x0085;
    cfg.GlyphOffset.y = 1.0f;
    cfg.GlyphRanges = atlas->GetGlyphRangesDefault();
    cfg.FontData = (void*)no_file;
    cfg.FontDataSize = sizeof no_file;
    cfg.FontDataOwnedByAtlas = false;
    ImFont* font = atlas->AddFont(&cfg);
    font->FontSize = cfg.SizePixels;
    font->ContainerAtlas = atlas;
    font->Ascent = f[0], font->Descent = f[1];
    font->MetricsTotalSurface = BAKED_SURFACE;
    font->Glyphs.resize(BAKED_GLYPHS);
    memcpy(font->Glyphs.Data, baked_glyphs, sizeof baked_glyphs);
    font->BuildLookupTable();
    for (int i = 0; i < 2; i++) {
        ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(atlas->AddCustomRectRegular(baked_rects[i][0], baked_rects[i][1]));
        r->X = baked_rects[i][2], r->Y = baked_rects[i][3];
    }
    atlas->PackIdMouseCursors = BAKED_CURSORS, atlas->PackIdLines = BAKED_LINES;
    atlas->TexWidth = BAKED_W, atlas->TexHeight = BAKED_H;
    atlas->TexUvScale = ImVec2(f[2], f[3]), atlas->TexUvWhitePixel = ImVec2(f[4], f[5]);
    memcpy(atlas->TexUvLines, f + 6, sizeof atlas->TexUvLines);
    atlas->TexReady = true;
    return true;
}

static int unlike(const void* a, const void* b, size_t bytes) { return memcmp(a, b, bytes) != 0; }

// IMGUI_FONT_CHECK=1: the atlas builds the font as it used to, and every part is compared.
static void check_font(ImFontAtlas* have)
{
    ImFontAtlas built;
    unsigned char* pixels;
    int width, height;
    built.GetTexDataAsAlpha8(&pixels, &width, &height);
    const ImFont *a = have->Fonts[0], *b = built.Fonts[0];
    const ImFontConfig *c = a->ConfigData, *d = b->ConfigData;
    int picture = width != BAKED_W || height != BAKED_H || unlike(pixels, baked_alpha, sizeof baked_alpha);
    int glyphs = a->Glyphs.Size != b->Glyphs.Size || unlike(a->Glyphs.Data, b->Glyphs.Data, a->Glyphs.Size * sizeof(ImFontGlyph));
    int tables = a->IndexAdvanceX.Size != b->IndexAdvanceX.Size || a->IndexLookup.Size != b->IndexLookup.Size ||
                 unlike(a->IndexAdvanceX.Data, b->IndexAdvanceX.Data, a->IndexAdvanceX.Size * sizeof(float)) ||
                 unlike(a->IndexLookup.Data, b->IndexLookup.Data, a->IndexLookup.Size * sizeof(ImWchar)) ||
                 unlike(a->Used4kPagesMap, b->Used4kPagesMap, sizeof a->Used4kPagesMap);
    int font = a->FallbackAdvanceX != b->FallbackAdvanceX || a->FontSize != b->FontSize || a->FallbackChar != b->FallbackChar ||
               a->FallbackGlyph - a->Glyphs.Data != b->FallbackGlyph - b->Glyphs.Data || a->EllipsisChar != b->EllipsisChar ||
               a->EllipsisCharCount != b->EllipsisCharCount || a->EllipsisWidth != b->EllipsisWidth ||
               a->EllipsisCharStep != b->EllipsisCharStep || a->Scale != b->Scale || a->Ascent != b->Ascent ||
               a->Descent != b->Descent || a->MetricsTotalSurface != b->MetricsTotalSurface ||
               a->ConfigDataCount != b->ConfigDataCount || a->DirtyLookupTables != b->DirtyLookupTables ||
               a->ContainerAtlas != have || have->Fonts.Size != built.Fonts.Size;
    int atlas = have->TexWidth != built.TexWidth || have->TexHeight != built.TexHeight || unlike(&have->TexUvScale, &built.TexUvScale, 8) ||
                unlike(&have->TexUvWhitePixel, &built.TexUvWhitePixel, 8) ||
                unlike(have->TexUvLines, built.TexUvLines, sizeof have->TexUvLines) ||
                have->PackIdMouseCursors != built.PackIdMouseCursors || have->PackIdLines != built.PackIdLines ||
                have->CustomRects.Size != built.CustomRects.Size || have->TexReady != built.TexReady ||
                have->ConfigData.Size != built.ConfigData.Size;
    for (int i = 0; i < have->CustomRects.Size && !atlas; i++) {
        const ImFontAtlasCustomRect &r = have->CustomRects[i], &t = built.CustomRects[i];
        atlas = r.Width != t.Width || r.Height != t.Height || r.X != t.X || r.Y != t.Y || r.GlyphID != t.GlyphID || r.Font || t.Font;
    }
    int settings = strcmp(c->Name, d->Name) || c->SizePixels != d->SizePixels || c->OversampleH != d->OversampleH ||
                   c->OversampleV != d->OversampleV || c->PixelSnapH != d->PixelSnapH || unlike(&c->GlyphOffset, &d->GlyphOffset, 8) ||
                   unlike(&c->GlyphExtraSpacing, &d->GlyphExtraSpacing, 8) || c->GlyphRanges != d->GlyphRanges ||
                   c->GlyphMinAdvanceX != d->GlyphMinAdvanceX || c->GlyphMaxAdvanceX != d->GlyphMaxAdvanceX ||
                   c->MergeMode != d->MergeMode || c->FontBuilderFlags != d->FontBuilderFlags || c->FontNo != d->FontNo ||
                   c->RasterizerMultiply != d->RasterizerMultiply || c->RasterizerDensity != d->RasterizerDensity ||
                   c->EllipsisChar != d->EllipsisChar || c->DstFont != a || d->DstFont != b;
    if (picture | glyphs | tables | font | atlas | settings)
        fprintf(stderr, "imgui: the font written down is NOT the one the atlas builds: picture %d, glyphs %d, tables %d, font %d, atlas %d, settings %d\n",
                picture, glyphs, tables, font, atlas, settings);
    else
        fprintf(stderr, "imgui: the font written down is the one the atlas builds (%d x %d, %d glyphs)\n", width, height, a->Glyphs.Size);
}

bool ImGui_ImplShaderEmu_Init(unsigned int nano_x_window)
{
    ImGuiIO& io = ImGui::GetIO();
    window = nano_x_window;
    io.BackendPlatformName = io.BackendRendererName = "imgui_impl_shaderemu";
    GrSelectEvents(window, GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP |
                               GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_POSITION |
                               GR_EVENT_MASK_MOUSE_EXIT);
    glGenTextures(1, &font_texture);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    if (!io.Fonts->IsBuilt() && baked_font(io.Fonts)) {
        // the device's words: white, and how transparent in the top byte. Most of the picture is empty.
        ImU32 *texels = (ImU32*)seglMemory(BAKED_W * BAKED_H * 4), *to = texels;
        if (!texels) return false;
        for (unsigned four : baked_alpha) {
            if (!four) to[0] = to[1] = to[2] = to[3] = ~0u;
            else to[0] = ~(four << 24), to[1] = ~(four >> 8 << 24), to[2] = ~(four >> 16 << 24), to[3] = ~(four >> 24 << 24);
            to += 4;
        }
        seglTexturePointer(texels, BAKED_W, BAKED_H, GL_RGBA);
        if (getenv("IMGUI_FONT_CHECK")) check_font(io.Fonts);
    } else {
        // a font of the program's own: bytes of red, green, blue and alpha, which the library makes the device's words
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        io.Fonts->ClearTexData();
    }
    io.Fonts->SetTexID((ImTextureID)font_texture);
    gettimeofday(&last_frame, nullptr);
    return font_texture != 0;
}

bool ImGui_ImplShaderEmu_NewFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    GR_EVENT event;
    bool open = true;
    // The window system is asked for events when the input words have changed, a few frames
    // more (an event may be on its way) and twice a second: a question that has no answer
    // is two system calls and two changes of task all the same.
    const volatile unsigned* input = control(0x20);
    const unsigned clock = *control(0x34);
    for (int i = 0; i < 4; i++)
        if (seen[i] != input[i]) seen[i] = input[i], asks = 8;
    if (!asks && clock - asked_at >= 500) asks = 1;
    while (asks) {
        GrCheckNextEvent(&event);
        if (event.type == GR_EVENT_TYPE_NONE) {
            asks--, asked_at = clock;
            break;
        }
        switch (event.type) {
            case GR_EVENT_TYPE_CLOSE_REQ: open = false; break;
            case GR_EVENT_TYPE_UPDATE:
                if (event.update.utype == GR_UPDATE_SIZE) seglWindowChanged();
                break;
            case GR_EVENT_TYPE_MOUSE_POSITION: io.AddMousePosEvent((float)event.mouse.x, (float)event.mouse.y); break;
            case GR_EVENT_TYPE_MOUSE_EXIT: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); break;
            case GR_EVENT_TYPE_BUTTON_DOWN:
            case GR_EVENT_TYPE_BUTTON_UP: {
                bool down = event.type == GR_EVENT_TYPE_BUTTON_DOWN;
                // a notch of the wheel is a button going down (docs/machine/input.md)
                if (event.button.buttons & GR_BUTTON_SCROLLUP) io.AddMouseWheelEvent(0, 1);
                if (event.button.buttons & GR_BUTTON_SCROLLDN) io.AddMouseWheelEvent(0, -1);
                io.AddMousePosEvent((float)event.button.x, (float)event.button.y);
                if (event.button.changebuttons & GR_BUTTON_L) io.AddMouseButtonEvent(0, down);
                if (event.button.changebuttons & GR_BUTTON_R) io.AddMouseButtonEvent(1, down);
                if (event.button.changebuttons & GR_BUTTON_M) io.AddMouseButtonEvent(2, down);
                break;
            }
            case GR_EVENT_TYPE_KEY_DOWN: key_event(event.keystroke, true); break;
            case GR_EVENT_TYPE_KEY_UP: key_event(event.keystroke, false); break;
        }
    }
    int width, height;
    seglSize(&width, &height);
    io.DisplaySize = ImVec2((float)width, (float)height);
    struct timeval now;
    gettimeofday(&now, nullptr);
    float passed = (float)(now.tv_sec - last_frame.tv_sec) + (float)(now.tv_usec - last_frame.tv_usec) * 1e-6f;
    io.DeltaTime = passed > 0 ? passed : 1.0f / 60;
    last_frame = now;
    return open;
}

// ---- drawing ----

// A corner as ImGui has it (ImDrawVert's layout).
struct corner { float x, y, u, v; ImU32 colour; };

// ---- drawn later (docs/apps/imgui.md, "Drawn later") ----
//
// With worker cores, a list's text, lines and filled shapes are not made by the first core
// (imgui.patch's hooks): it keeps what each was asked with, the text or the points, in the
// list's _Deferred words, and puts in the list a triangle on one vertex three times, which
// stands for it. The core that copies the list makes it there, by the library's own function
// on a list of its own (the same triangles in the same order), and copies those.
//
// The vertex: x is all ones (no corner is inside a clip rectangle so, and the fast loops leave
// it to one_shape()), y the record's place in _Deferred, u the indices the library could make of
// it at most (the copy's room) and v what making it costs, in about four instructions (an index
// copied is one): the workers' shares are cut by that.
enum { DEFER_TEXT = 1, DEFER_LINE, DEFER_FILL, DEFER_SEGMENTS, DEFER_PLOT, DEFER_PLOT_PART };
#define SCRATCH_V 2048      // a list of a copier's own, for one primitive at a time; one larger is made at once
#define SCRATCH_I 6144
#define SCRATCH_T 2048      // the library's temporary points (ImDrawListSharedData::TempBuffer)
#define MARK 0xffffffffu

struct text_record { ImU32 kind; ImFont* font; float size; ImVec2 pos; ImU32 col; ImVec4 clip; float wrap; ImU32 fine, length; char text[]; };
struct shape_record { ImU32 kind, col; ImDrawFlags flags; float thickness; ImDrawListFlags list_flags; float fringe; ImU32 count; ImVec2 points[]; };
struct plot_record {
    ImU32 kind; int values_count, values_offset, res_w, item_count; float scale_min, scale_max; ImVec2 min, max;
    ImU32 col_base, col_hovered; int idx_hovered; ImDrawListFlags list_flags; float fringe; int from, to; float values[];
};
// a later part of a plot's segments, `from` to `to`, by the plot's record at `whole`
struct plot_part { ImU32 kind; int whole, from, to; };

static bool deferring;

// Room for a record at the end of the list's words. (Its growth apart: a hook's own work is a
// few dozen instructions, and a call with the growth in it would save registers every time.)
__attribute__((noinline)) static void defer_grow(ImVector<unsigned int>& d, int size) { d.reserve(d._grow_capacity(size)); }

static inline ImU32* defer_room(ImDrawList* list, int words, int* at)
{
    ImVector<unsigned int>& d = list->_Deferred;
    const int size = d.Size + words;
    if (size > d.Capacity) defer_grow(d, size);
    *at = d.Size;
    d.Size = size;
    return d.Data + *at;
}

// What stands for a record in the list: one vertex, three indices, as PrimReserve() would make
// room for them (it does when the list is full or near 65,536 vertices).
static inline void defer_mark(ImDrawList* list, int at, int indices, int work)
{
    ImDrawList& l = *list;
    ImU32* v;
    ImDrawIdx* i;
    if (l.VtxBuffer.Size < l.VtxBuffer.Capacity && l.IdxBuffer.Size + 3 <= l.IdxBuffer.Capacity && l._VtxCurrentIdx < 0xfff0) {
        v = (ImU32*)(l.VtxBuffer.Data + l.VtxBuffer.Size), i = l.IdxBuffer.Data + l.IdxBuffer.Size;
        l.CmdBuffer.Data[l.CmdBuffer.Size - 1].ElemCount += 3;
        l.VtxBuffer.Size++, l.IdxBuffer.Size += 3;
    } else {
        l.PrimReserve(3, 1);
        v = (ImU32*)l._VtxWritePtr, i = l._IdxWritePtr;
    }
    // (the command's work and corners, for the workers' shares: imgui.patch adds them up where commands are made one)
    ImDrawCmd& cmd = l.CmdBuffer.Data[l.CmdBuffer.Size - 1];
    cmd.UserCallbackData = (char*)cmd.UserCallbackData + work, cmd.UserCallbackDataSize += indices;
    v[0] = MARK, v[1] = (ImU32)at, v[2] = (ImU32)indices, v[3] = (ImU32)work, v[4] = 0;
    i[0] = i[1] = i[2] = (ImDrawIdx)l._VtxCurrentIdx;
    l._VtxWritePtr = (ImDrawVert*)v + 1, l._IdxWritePtr = i + 3, l._VtxCurrentIdx++;
}

static bool defer_text(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, ImU32 col, const ImVec4& clip, const char* begin,
                       const char* end, float wrap, bool fine)
{
    const int length = (int)(end - begin);
    if (length > SCRATCH_V / 4) return false;
    if ((float)(int)pos.y > clip.w) return true;    // (RenderText's own first test: nothing is drawn)
    int at;
    text_record* r = (text_record*)defer_room(list, (int)(sizeof(text_record) + length + 3) / 4, &at);
    r->kind = DEFER_TEXT, r->font = font, r->size = size, r->pos = pos, r->col = col, r->clip = clip, r->wrap = wrap;
    r->fine = fine, r->length = (ImU32)length;
    memcpy(r->text, begin, (size_t)length);
    defer_mark(list, at, 6 * length, 22 * length + 30);
    return true;
}

static bool defer_shape(ImDrawList* list, ImU32 kind, const ImVec2* points, int count, ImU32 col, ImDrawFlags flags, float thickness,
                        int indices, int vertices, int temporary, int work)
{
    if (vertices > SCRATCH_V || indices > SCRATCH_I || temporary > SCRATCH_T) return false;
    int at;
    shape_record* r = (shape_record*)defer_room(list, (int)(sizeof(shape_record) / 4) + 2 * count, &at);
    r->kind = kind, r->col = col, r->flags = flags, r->thickness = thickness;
    r->list_flags = list->Flags & ~ImDrawListFlags_Deferred, r->fringe = list->_FringeScale, r->count = (ImU32)count;
    memcpy(r->points, points, (size_t)count * sizeof(ImVec2));
    defer_mark(list, at, indices, work);
    return true;
}

// (at most: a thick line's 18 indices a segment and four vertices a point, five temporary points a point)
static bool defer_line(ImDrawList* list, const ImVec2* points, int count, ImU32 col, ImDrawFlags flags, float thickness)
{
    return defer_shape(list, DEFER_LINE, points, count, col, flags, thickness, 18 * count, 4 * count, 5 * count, 30 + 30 * count);
}

static bool defer_fill(ImDrawList* list, const ImVec2* points, int count, ImU32 col)
{
    return defer_shape(list, DEFER_FILL, points, count, col, 0, 0, 9 * count, 2 * count, 2 * count, 30 + 25 * count);
}

// AddLine's: one-segment lines of one colour and width, one after another in one command with
// nothing else drawn between them (a plot's), are one record of their segments, each a polyline
// of its two points on the copier's list, as each was (at most 18 indices and 8 vertices a
// segment). The record before is forgotten when a frame is copied.
#define LINES_MOST 128
#define SEGMENT_WORK 150    // (a two-point polyline is some 400 instructions, and its copy: 600 as measured)
static struct {
    const ImDrawList* list;
    const ImDrawIdx* index_end;     // the list's when the last segment was taken
    int records_end, commands, at, left;
    ImU32 col;
    float thickness, fringe;
    ImDrawListFlags flags;
} lines_last;

__attribute__((noinline)) static bool lines_begin(ImDrawList* list, float x1, float y1, float x2, float y2, ImU32 col, float thickness)
{
    int at;
    shape_record* r = (shape_record*)defer_room(list, (int)(sizeof(shape_record) / 4) + 4, &at);
    r->kind = DEFER_SEGMENTS, r->col = col, r->flags = 0, r->thickness = thickness;
    r->list_flags = list->Flags & ~ImDrawListFlags_Deferred, r->fringe = list->_FringeScale, r->count = 2;
    r->points[0] = ImVec2(x1, y1), r->points[1] = ImVec2(x2, y2);
    defer_mark(list, at, 18, SEGMENT_WORK);
    lines_last = {list, list->_IdxWritePtr, list->_Deferred.Size, list->CmdBuffer.Size, at, LINES_MOST - 1, col, thickness, list->_FringeScale, list->Flags};
    return true;
}

static bool defer_segment(ImDrawList* list, float x1, float y1, float x2, float y2, ImU32 col, float thickness)
{
    ImVector<unsigned int>& d = list->_Deferred;
    if (lines_last.list != list || lines_last.index_end != list->_IdxWritePtr || lines_last.records_end != d.Size ||
        lines_last.commands != list->CmdBuffer.Size || lines_last.col != col || lines_last.thickness != thickness ||
        lines_last.flags != list->Flags || lines_last.fringe != list->_FringeScale || !lines_last.left || d.Size + 4 > d.Capacity)
        return lines_begin(list, x1, y1, x2, y2, col, thickness);
    float* to = (float*)(d.Data + d.Size);
    to[0] = x1, to[1] = y1, to[2] = x2, to[3] = y2;
    d.Size += 4, lines_last.records_end = d.Size, lines_last.left--;
    ((shape_record*)(d.Data + lines_last.at))->count += 2;
    ImU32* mark = (ImU32*)(list->_VtxWritePtr - 1);
    mark[2] += 18, mark[3] += SEGMENT_WORK;
    ImDrawCmd& cmd = list->CmdBuffer.Data[list->CmdBuffer.Size - 1];
    cmd.UserCallbackData = (char*)cmd.UserCallbackData + SEGMENT_WORK, cmd.UserCallbackDataSize += 18;
    return true;
}

// PlotLines()'s segments (imgui.patch, PlotEx()): its values copied, and the scale (when it is
// to be found from them) and every segment worked out where the record is made, as PlotEx() would.
// The segments are in parts of PLOT_PART, each a record and a marker of its own, so that several
// cores make one plot: a part goes through the segments before it for their floats alone.
#define PLOT_PART 12
static bool defer_plot(ImDrawList* list, float (*getter)(void*, int), void* data, const float* array, int stride, int count, int offset,
                       int res_w, int item_count, float scale_min, float scale_max, const ImVec2& min, const ImVec2& max, ImU32 col_base,
                       ImU32 col_hovered, int idx_hovered)
{
    if (res_w <= 0 || 8 * res_w > SCRATCH_V || 18 * res_w > SCRATCH_I) return false;
    int at;
    plot_record* r = (plot_record*)defer_room(list, (int)(sizeof(plot_record) / 4) + count, &at);
    r->kind = DEFER_PLOT, r->values_count = count, r->values_offset = offset, r->res_w = res_w, r->item_count = item_count;
    r->scale_min = scale_min, r->scale_max = scale_max, r->min = min, r->max = max, r->col_base = col_base, r->col_hovered = col_hovered;
    r->idx_hovered = idx_hovered, r->list_flags = list->Flags & ~ImDrawListFlags_Deferred, r->fringe = list->_FringeScale;
    if (array && stride == 4) memcpy(r->values, array, (size_t)count * 4);
    else if (array) for (int i = 0; i < count; i++) r->values[i] = *(const float*)(const void*)((const char*)array + (size_t)i * stride);
    else for (int i = 0; i < count; i++) r->values[i] = getter(data, i);
    r->from = 0, r->to = res_w < PLOT_PART ? res_w : PLOT_PART;
    defer_mark(list, at, 18 * r->to, SEGMENT_WORK * r->to + count);
    for (int from = PLOT_PART; from < res_w; from += PLOT_PART) {
        const int whole = at, to = from + PLOT_PART < res_w ? from + PLOT_PART : res_w;
        plot_part* part = (plot_part*)defer_room(list, (int)(sizeof(plot_part) / 4), &at);
        part->kind = DEFER_PLOT_PART, part->whole = whole, part->from = from, part->to = to;
        defer_mark(list, at, 18 * (to - from), SEGMENT_WORK * (to - from) + count + 5 * from);
        at = whole;
    }
    return true;
}

// PlotEx()'s loop, from its record: the same floats and the same AddLine() calls, of the
// segments from `from` to `to`.
static void plot_made(ImDrawList& l, const plot_record& r, int from, int to)
{
    const float* v = r.values;
    const int count = r.values_count, offset = r.values_offset;
    float scale_min = r.scale_min, scale_max = r.scale_max;
    if (scale_min == FLT_MAX || scale_max == FLT_MAX) {
        float v_min = FLT_MAX, v_max = -FLT_MAX;
        for (int i = 0; i < count; i++) {
            if (v[i] != v[i]) continue;
            v_min = ImMin(v_min, v[i]), v_max = ImMax(v_max, v[i]);
        }
        if (scale_min == FLT_MAX) scale_min = v_min;
        if (scale_max == FLT_MAX) scale_max = v_max;
    }
    const float t_step = 1.0f / (float)r.res_w;
    const float inv_scale = (scale_min == scale_max) ? 0.0f : (1.0f / (scale_max - scale_min));
    float t0 = 0.0f;
    ImVec2 tp0 = ImVec2(t0, 1.0f - ImSaturate((v[(0 + offset) % count] - scale_min) * inv_scale));
    for (int n = 0; n < to; n++) {
        const float t1 = t0 + t_step;
        const int v1_idx = (int)(t0 * r.item_count + 0.5f);
        const float v1 = v[(v1_idx + offset + 1) % count];
        const ImVec2 tp1 = ImVec2(t1, 1.0f - ImSaturate((v1 - scale_min) * inv_scale));
        if (n >= from) l.AddLine(ImLerp(r.min, r.max, tp0), ImLerp(r.min, r.max, tp1), r.idx_hovered == v1_idx ? r.col_hovered : r.col_base);
        t0 = t1, tp0 = tp1;
    }
}

// A copier's own list and the library's shared data it makes them with: its memory here (a
// worker allocates nothing, and the program's data is touched for the workers before they start),
// the shared data a copy of the context's made by the first core (the temporary points the copier's).
struct alignas(16) scratch {      // (16 bytes apart: each a worker's, stored to in the same passes)
    ImDrawVert vertices[SCRATCH_V];
    ImDrawIdx indices[SCRATCH_I];
    ImVec2 temporary[SCRATCH_T];
    ImDrawCmd commands[2];
    ImVec2 path[16];        // (AddLine's two points)
    alignas(16) unsigned char list[sizeof(ImDrawList)], shared[sizeof(ImDrawListSharedData)];
    bool made;
};

static void scratch_ready(scratch* s, const ImDrawListSharedData* live)
{
    ImDrawListSharedData* d = (ImDrawListSharedData*)s->shared;
    ImDrawList* l = (ImDrawList*)s->list;
    if (!s->made) IM_PLACEMENT_NEW(l) ImDrawList(d);
    // (what the three functions read of it; the tables follow the circles' error)
    if (!s->made || d->TexUvWhitePixel.x != live->TexUvWhitePixel.x || d->TexUvWhitePixel.y != live->TexUvWhitePixel.y ||
        d->TexUvLines != live->TexUvLines || d->Font != live->Font || d->FontSize != live->FontSize ||
        d->CircleSegmentMaxError != live->CircleSegmentMaxError || d->CurveTessellationTol != live->CurveTessellationTol ||
        d->ArcFastRadiusCutoff != live->ArcFastRadiusCutoff) {
        memcpy((void*)d, (const void*)live, sizeof *d);
        d->TempBuffer.Data = s->temporary, d->TempBuffer.Capacity = SCRATCH_T, d->TempBuffer.Size = 0;
    }
    if (!s->made) {
        l->VtxBuffer.Data = s->vertices, l->VtxBuffer.Capacity = SCRATCH_V;
        l->IdxBuffer.Data = s->indices, l->IdxBuffer.Capacity = SCRATCH_I;
        l->CmdBuffer.Data = s->commands, l->CmdBuffer.Capacity = 2;
        l->_Path.Data = s->path, l->_Path.Capacity = 16;
        s->made = true;
    }
}

// A piece of one of ImGui's commands: its clip rectangle and texture, where its vertices are,
// and the indices of it that are this piece's; its list's records.
struct piece { ImVec4 clip; GLuint name; const char* base; const ImDrawIdx *index, *end; const ImU32* records; };
// Vertices a worker core wrote, for the first core to make a command of.
struct run { ImU32 *first, *table; int count, quads; GLuint name; };
#define MOST_RUNS 256

// A texture coordinate (a float's bits) in 1,024ths.
static inline ImU32 texels(ImU32 coordinate)
{
    float f;
    memcpy(&f, &coordinate, 4);
    return (ImU32)(int)(f * 1024.0f + 0.5f);
}

static inline ImU32* put(ImU32* to, const ImU32* from, ImU32 tag)
{
    to[0] = from[0], to[1] = from[1], to[2] = 0;
    to[3] = texels(from[2]) | texels(from[3]) << 12 | tag;
    return to + 4;
}

static corner between(const corner& a, const corner& b, float t)
{
    corner c = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t, a.colour};
    if (a.colour != b.colour) {
        c.colour = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            float from = (float)(a.colour >> shift & 255), to = (float)(b.colour >> shift & 255);
            c.colour |= (ImU32)(from + (to - from) * t + 0.5f) << shift;
        }
    }
    return c;
}

// The part of a polygon on the inner side of one edge of the clip rectangle: x or y at least
// (or at most) `limit`. The device has no scissor, so a triangle that crosses one is cut here.
static int cut(const corner* in, int count, corner* out, bool is_y, bool at_least, float limit)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        const corner &a = in[i], &b = in[(i + 1) % count];
        float pa = is_y ? a.y : a.x, pb = is_y ? b.y : b.x;
        bool a_in = at_least ? pa >= limit : pa <= limit, b_in = at_least ? pb >= limit : pb <= limit;
        if (a_in) out[n++] = a;
        if (a_in != b_in) out[n++] = between(a, b, (limit - pa) / (pb - pa));
    }
    return n;
}

static inline float least(float a, float b, float c)
{
    float m = a < b ? a : b;
    return m < c ? m : c;
}

static inline ImU32 bits(float f)
{
    ImU32 u;
    memcpy(&u, &f, 4);
    return u;
}

// The clip rectangle is kept as the bits of its floats, which are in the floats' order when
// none is negative: a corner is inside when its own bits are within `wide` and `high` of the
// left and the top. (With 0 and 0 no corner is, and the floats are asked.)
static inline bool inside(const ImU32* corner, ImU32 left, ImU32 top, ImU32 wide, ImU32 high)
{
    return corner[0] - left < wide && corner[1] - top < high;
}

// The copy and all it keeps, so that several cores can each make one (docs/apps/imgui.md, "The copy
// on worker cores"). A vertex goes to the GPU as four words (docs/machine/gpu.md, a tagged vertex):
// ImGui's x and y as the floats they are, 0, and the texture coordinates in 1,024ths with a
// tag, the number of the vertex's colour in a table of the colours the copy has used.
struct alignas(16) copier {
    ImU32* table;                           // the table being filled: 256 words in the frame
    int colours;                            // how many it has
    ImU32 slot_colour[256];                 // the table's colours by a hash of them
    unsigned char slot_tag[256], slot_used[256];
    // The copy's own state, kept here between its pieces (docs/apps/imgui.md, "The copy").
    struct {
        ImU32 *to, *first, *last;   // the next corner's place, the run's first, the last place a cut triangle may begin
        int quads;                  // the run being written: four corners a quad (1), or three a triangle (0)
        GLuint name;                // the texture of ImGui's command
        ImU32 colour[3], tag[3];    // the colours looked up last, the newest first, and their tags as a vertex has them:
        bool tagged;                // (once there are any) a bar's, its text's, and what a shape's edge fades to
        ImU32 left, top, wide, high;    // the command's clip rectangle, for inside() below
        const ImDrawIdx* next_lone; // triangles one after the other in a run of quads: where the next would begin,
        int lone;                   // and how many there are so far
        const ImU32* records;       // the list's records ("Drawn later")
    } out;
    scratch* later;                 // its own list for them
    // On a worker core (`ahead`): room and tables of its own in the frame, the pieces that are
    // its share, and the runs it wrote. `noted`: the first core's runs are kept too (the sum).
    bool ahead, noted, failed;
    ImU32 *room, *room_end, *tables;
    int tables_left, run_count;
    const piece *pieces, *pieces_end;
    run runs[MOST_RUNS];

    bool new_table();
    void more_room();
    void run_done();
    void flush(int next_quads);
    bool full_table();
    bool recalled(ImU32 colour);
    bool coloured(const ImDrawIdx* index, const ImU32* p, const ImU32* q, const ImU32* r, const ImU32* s);
    bool edge_triangle(const ImVec4& clip, const ImDrawIdx* index, const ImU32* p, const ImU32* q, const ImU32* r);
    int edge_quad(const ImVec4& clip, const ImDrawIdx* index, const ImU32* p, const ImU32* q, const ImU32* r, const ImU32* s);
    int one_shape(const ImVec4& clip, const char* base, const ImDrawIdx* index, const ImDrawIdx* end);
    bool made_later(const ImVec4& clip, const ImU32* mark);
    bool copy_command(const ImVec4& clip, const char* base, const ImDrawIdx* index, const ImDrawIdx* end);
    void copy_pieces();

    // A colour and its tag become the newest of the three remembered, in place of entry `old`.
    inline void newest(ImU32 colour, ImU32 tag, int old)
    {
        for (; old > 0; old--) out.colour[old] = out.colour[old - 1], out.tag[old] = out.tag[old - 1];
        out.colour[0] = colour, out.tag[0] = tag;
    }

    // The tag of a colour as a vertex has it, into `word`. False when the table is full.
    inline bool tag_of(ImU32 colour, ImU32* word)
    {
        if (out.tagged) {
            if (colour == out.colour[0]) return *word = out.tag[0], true;
            if (recalled(colour)) return *word = out.tag[0], true;
        }
        unsigned at = (colour * 2654435761u) >> 24;
        while (slot_used[at] && slot_colour[at] != colour) at = (at + 1) & 255;
        if (!slot_used[at]) {
            if (colours == 256) return false;
            slot_used[at] = 1;
            slot_colour[at] = colour;
            slot_tag[at] = (unsigned char)colours;
            // ImGui's is alpha, blue, green, red from the top; the device's transparency, red, green, blue
            table[colours++] = (255 - (colour >> 24)) << 24 | (colour & 255) << 16 | (colour & 0xff00) | (colour >> 16 & 255);
        }
        *word = (ImU32)slot_tag[at] << 24;
        // (all three: one left from the table before would be found again with that table's tag)
        if (!out.tagged) out.colour[0] = out.colour[1] = out.colour[2] = colour, out.tag[0] = out.tag[1] = out.tag[2] = *word, out.tagged = true;
        newest(colour, *word, 2);
        return true;
    }

    // A few triangles among quads (an arrow in a list) are quads whose last two corners are one,
    // which draws the triangle and nothing more: no run of their own, and none after them.
    // Many in a row (a plot's line) are a run. True: the triangle at `index` is to be a quad.
    inline bool lone_as_quad(const ImDrawIdx* index)
    {
        if (index != out.next_lone) out.lone = 0;
        out.next_lone = index + 3;
        return ++out.lone <= 16;
    }
};

bool copier::new_table()
{
    if (ahead) {
        // (a worker has the tables it was given: with none left its share is the first core's)
        table = tables_left ? tables : nullptr;
        tables += 256, tables_left -= tables_left ? 1 : 0;
        if (!table) failed = true;
    } else {
        table = (ImU32*)seglScreenTable();
    }
    colours = 0;
    out.tagged = false;
    memset(slot_used, 0, sizeof slot_used);
    return table != nullptr;
}

// Is `colour` one of the two remembered before the newest? Then it is the newest now.
__attribute__((noinline)) bool copier::recalled(ImU32 colour)
{
    for (int i = 1; i < 3; i++)
        if (colour == out.colour[i]) return newest(colour, out.tag[i], i), true;
    return false;
}

void copier::more_room()
{
    if (ahead) {
        out.to = out.first = room;
        out.last = room_end - 4 * 24;
        return;
    }
    GLsizei count = 0;
    out.to = out.first = (ImU32*)seglScreenCompactSpace(&count);
    out.last = out.to + 4 * (count - 24);    // (a triangle cut into a fan is 24 corners at most)
}

// What is written of the run becomes a command of the device's: now, or on a worker noted
// for the first core to make one of.
void copier::run_done()
{
    const int count = (int)(out.to - out.first) / 4;
    if ((ahead || noted) && count > 0) {
        if (run_count == MOST_RUNS) failed = true;
        else runs[run_count++] = {out.first, table, count, out.quads, out.name};
    }
    if (ahead) room = out.to;
    else seglScreenCompactUsed(count, out.quads, out.name, (const unsigned int*)table);
}

// The run ends, and one of quads or of triangles begins in the room there is now.
void copier::flush(int next_quads)
{
    run_done();
    out.quads = next_quads;
    more_room();
}

bool copier::full_table()
{
    run_done();
    if (!new_table()) return false;
    more_room();
    return true;
}

// A quad or (no `s`) a triangle that is all inside its clip rectangle, the long way: its
// colours looked up, and a run of the other kind begun if it must be. False: the table is full.
bool copier::coloured(const ImDrawIdx* index, const ImU32* p, const ImU32* q, const ImU32* r, const ImU32* s)
{
    const bool quad = s != nullptr;
    ImU32 a, b, c, d;
    if (!quad) s = r;
    // (in the corners' order: the table has its colours in the order they were first seen)
    bool fit = tag_of(p[4], &a);
    fit &= tag_of(q[4], &b);
    fit &= tag_of(r[4], &c);
    fit &= tag_of(s[4], &d);
    if (!fit) return false;
    const bool as_quad = quad || (out.quads && lone_as_quad(index));
    if (out.quads != as_quad) flush(as_quad);
    ImU32* to = put(put(put(out.to, p, a), q, b), r, c);
    out.to = as_quad ? put(to, s, d) : to;
    return true;
}

// A triangle that may reach over its clip rectangle: by its box, drawn whole, or left out, or
// cut by each edge in turn (up to seven corners, drawn as a fan). False when the table is full.
bool copier::edge_triangle(const ImVec4& clip, const ImDrawIdx* index, const ImU32* p, const ImU32* q, const ImU32* r)
{
    const corner *a = (const corner*)p, *b = (const corner*)q, *c = (const corner*)r;
    float x0 = least(a->x, b->x, c->x), x1 = -least(-a->x, -b->x, -c->x);
    float y0 = least(a->y, b->y, c->y), y1 = -least(-a->y, -b->y, -c->y);
    if (x0 >= clip.x && x1 <= clip.z && y0 >= clip.y && y1 <= clip.w) return coloured(index, p, q, r, nullptr);
    if (!(x1 >= clip.x && x0 <= clip.z && y1 >= clip.y && y0 <= clip.w)) return true;
    corner tri[3] = {*a, *b, *c}, even[8], odd[8];
    int count = cut(tri, 3, even, false, true, clip.x);
    ImU32 fan[8];
    bool fits = true;
    count = cut(even, count, odd, false, false, clip.z);
    count = cut(odd, count, even, true, true, clip.y);
    count = cut(even, count, odd, true, false, clip.w);
    for (int k = 0; k < count; k++) fits &= tag_of(odd[k].colour, &fan[k]);
    if (!fits) return false;
    if (out.quads) flush(0);
    for (int k = 1; k + 1 < count; k++)
        out.to = put(put(put(out.to, (const ImU32*)&odd[0], fan[0]), (const ImU32*)&odd[k], fan[k]), (const ImU32*)&odd[k + 1], fan[k + 1]);
    return true;
}

// A quad that may reach over its clip rectangle, by its box. 1: drawn whole, or left out;
// 0: its two triangles are to be looked at one by one; -1: the table is full.
int copier::edge_quad(const ImVec4& clip, const ImDrawIdx* index, const ImU32* p, const ImU32* q, const ImU32* r, const ImU32* s)
{
    const corner *a = (const corner*)p, *b = (const corner*)q, *c = (const corner*)r, *d = (const corner*)s;
    float x0 = least(a->x, b->x, c->x), x1 = -least(-a->x, -b->x, -c->x);
    float y0 = least(a->y, b->y, c->y), y1 = -least(-a->y, -b->y, -c->y);
    x0 = x0 < d->x ? x0 : d->x, x1 = x1 > d->x ? x1 : d->x;
    y0 = y0 < d->y ? y0 : d->y, y1 = y1 > d->y ? y1 : d->y;
    if (x0 >= clip.x && x1 <= clip.z && y0 >= clip.y && y1 <= clip.w) return coloured(index, p, q, r, s) ? 1 : -1;
    return !(x1 >= clip.x && x0 <= clip.z && y1 >= clip.y && y0 <= clip.w);
}

// One triangle or quad the long way: whatever it is. How many indices it was, or 0 when the
// table is full.
__attribute__((noinline)) int copier::one_shape(const ImVec4& clip, const char* base, const ImDrawIdx* index, const ImDrawIdx* end)
{
    const ImU32 *p = (const ImU32*)(base + index[0] * sizeof(ImDrawVert)), *q = (const ImU32*)(base + index[1] * sizeof(ImDrawVert)),
                *r = (const ImU32*)(base + index[2] * sizeof(ImDrawVert));
    if (index[0] == index[1] && p[0] == MARK) return made_later(clip, p) ? 3 : 0;
    const ImU32 left = out.left, top = out.top, wide = out.wide, high = out.high;
    const bool three = inside(p, left, top, wide, high) && inside(q, left, top, wide, high) && inside(r, left, top, wide, high);
    // two triangles on corners 0 1 2 and 0 2 3, as ImGui makes its rectangles and letters: a quad
    if (index + 5 < end && index[3] == index[0] && index[4] == index[2]) {
        const ImU32* s = (const ImU32*)(base + index[5] * sizeof(ImDrawVert));
        if (three && inside(s, left, top, wide, high)) return coloured(index, p, q, r, s) ? 6 : 0;
        int whole = edge_quad(clip, index, p, q, r, s);
        if (whole) return whole > 0 ? 6 : 0;
    }
    return (three ? coloured(index, p, q, r, nullptr) : edge_triangle(clip, index, p, q, r)) ? 3 : 0;
}

// A record ("Drawn later") made by the library on the copier's own list, and copied as its
// command's triangles. False: no table.
__attribute__((noinline)) bool copier::made_later(const ImVec4& clip, const ImU32* mark)
{
    const ImU32* record = out.records + mark[1];
    ImDrawList& l = *(ImDrawList*)later->list;
    l.VtxBuffer.Size = l.IdxBuffer.Size = 0, l.CmdBuffer.Size = 1;
    later->commands[0].ElemCount = 0, later->commands[0].VtxOffset = 0, l._CmdHeader.VtxOffset = 0;
    l._VtxCurrentIdx = 0, l._VtxWritePtr = later->vertices, l._IdxWritePtr = later->indices;
    if (record[0] == DEFER_TEXT) {
        const text_record& t = *(const text_record*)record;
        t.font->RenderText(&l, t.size, t.pos, t.col, t.clip, t.text, t.text + t.length, t.wrap, t.fine != 0);
    } else if (record[0] == DEFER_PLOT || record[0] == DEFER_PLOT_PART) {
        const plot_part* part = record[0] == DEFER_PLOT_PART ? (const plot_part*)record : nullptr;
        const plot_record& p = *(const plot_record*)(part ? out.records + part->whole : record);
        l.Flags = p.list_flags, l._FringeScale = p.fringe;
        plot_made(l, p, part ? part->from : p.from, part ? part->to : p.to);
    } else {
        const shape_record& s = *(const shape_record*)record;
        l.Flags = s.list_flags, l._FringeScale = s.fringe;
        if (s.kind == DEFER_LINE) l.AddPolyline(s.points, (int)s.count, s.col, s.flags, s.thickness);
        else if (s.kind == DEFER_FILL) l.AddConvexPolyFilled(s.points, (int)s.count, s.col);
        else
            for (ImU32 i = 0; i < s.count; i += 2) l.AddPolyline(s.points + i, 2, s.col, 0, s.thickness);
    }
    if (!l.IdxBuffer.Size) return true;
    return copy_command(clip, (const char*)l.VtxBuffer.Data, l.IdxBuffer.Data, l.IdxBuffer.Data + l.IdxBuffer.Size);
}

// One of ImGui's commands, or a piece of one: its triangles into the frame, as the runs of
// quads and of triangles they make. Nearly all are inside the clip rectangle and of the two
// colours looked up last: those are written by the two loops here, a run of each kind, and
// the rest by one_shape(). False: no table.
__attribute__((noinline)) bool copier::copy_command(const ImVec4& clip, const char* base, const ImDrawIdx* index, const ImDrawIdx* const end)
{
    const ImU32 left = bits(clip.x), top = bits(clip.y), right = bits(clip.z), bottom = bits(clip.w);
    const bool ordered = !((left | top | right | bottom) >> 31) && right >= left && bottom >= top;
    const ImU32 wide = ordered ? right - left + 1 : 0, high = ordered ? bottom - top + 1 : 0;
    out.left = left, out.top = top, out.wide = wide, out.high = high;
    // (a multiplication is one instruction here, and the compiler would shift and add instead)
    ImU32 stride = sizeof(ImDrawVert);
    __asm__("" : "+r"(stride));
    for (;;) {
        ImU32* to = out.to;
        ImU32* const last = out.last;
        const ImU32 colour = out.colour[0], tag = out.tag[0];
        bool again = false, stopped = false;
        if (out.tagged && out.quads) {
            // two triangles on corners 0 1 2 and 0 2 3, as ImGui makes its rectangles and letters: a quad
            for (; index + 5 < end && to <= last && index[3] == index[0] && index[4] == index[2]; index += 6, to += 16) {
                const ImU32 *p = (const ImU32*)(base + index[0] * stride), *q = (const ImU32*)(base + index[1] * stride),
                            *r = (const ImU32*)(base + index[2] * stride), *s = (const ImU32*)(base + index[5] * stride);
                stopped = true;
                if (!(inside(p, left, top, wide, high) && inside(q, left, top, wide, high) && inside(r, left, top, wide, high) &&
                      inside(s, left, top, wide, high)))
                    break;
                if (p[4] != colour || q[4] != colour || r[4] != colour || s[4] != colour) {
                    // (a bar, its text, the next bar: the colours take turns)
                    again = q[4] == p[4] && r[4] == p[4] && s[4] == p[4] && recalled(p[4]);
                    break;
                }
                stopped = false;
                const ImU32 u0 = p[2], v0 = p[3], u1 = r[2], v1 = r[3];
                to[0] = p[0], to[1] = p[1], to[2] = 0;
                to[4] = q[0], to[5] = q[1], to[6] = 0;
                to[8] = r[0], to[9] = r[1], to[10] = 0;
                to[12] = s[0], to[13] = s[1], to[14] = 0;
                if (q[2] == u1 && q[3] == v0 && s[2] == u0 && s[3] == v1) {
                    // a rectangle of the texture, as a letter is: two coordinates across and two down
                    const ImU32 a = texels(u0) | tag, b = texels(u1) | tag, c = texels(v0) << 12, d = texels(v1) << 12;
                    to[3] = a | c, to[7] = b | c, to[11] = b | d, to[15] = a | d;
                } else {
                    to[3] = texels(u0) | texels(v0) << 12 | tag, to[7] = texels(q[2]) | texels(q[3]) << 12 | tag;
                    to[11] = texels(u1) | texels(v1) << 12 | tag, to[15] = texels(s[2]) | texels(s[3]) << 12 | tag;
                }
            }
            // a few triangles among the quads, as quads (lone_as_quad())
            if (!stopped) {
                const ImU32 other = out.colour[1], other_tag = out.tag[1];
                for (; index + 2 < end && to <= last; index += 3, to += 16) {
                    if (index + 5 < end && index[3] == index[0] && index[4] == index[2]) {
                        again = true;
                        break;
                    }
                    const ImU32 *p = (const ImU32*)(base + index[0] * stride), *q = (const ImU32*)(base + index[1] * stride),
                                *r = (const ImU32*)(base + index[2] * stride);
                    if (!(inside(p, left, top, wide, high) && inside(q, left, top, wide, high) && inside(r, left, top, wide, high))) break;
                    const ImU32 pc = p[4], qc = q[4], rc = r[4];
                    if ((pc != colour && pc != other) || (qc != colour && qc != other) || (rc != colour && rc != other)) break;
                    if (!lone_as_quad(index)) {
                        out.lone--, out.next_lone = index;      // (one_shape() counts it again)
                        break;
                    }
                    const ImU32 x = r[0], y = r[1], w = texels(r[2]) | texels(r[3]) << 12 | (rc == colour ? tag : other_tag);
                    to[0] = p[0], to[1] = p[1], to[2] = 0, to[3] = texels(p[2]) | texels(p[3]) << 12 | (pc == colour ? tag : other_tag);
                    to[4] = q[0], to[5] = q[1], to[6] = 0, to[7] = texels(q[2]) | texels(q[3]) << 12 | (qc == colour ? tag : other_tag);
                    to[8] = x, to[9] = y, to[10] = 0, to[11] = w;
                    to[12] = x, to[13] = y, to[14] = 0, to[15] = w;
                }
            }
        } else if (out.tagged) {
            const ImU32 other = out.colour[1], other_tag = out.tag[1];
            for (; index + 2 < end && to <= last; index += 3, to += 12) {
                if (index + 5 < end && index[3] == index[0] && index[4] == index[2]) break;
                const ImU32 *p = (const ImU32*)(base + index[0] * stride), *q = (const ImU32*)(base + index[1] * stride),
                            *r = (const ImU32*)(base + index[2] * stride);
                if (!(inside(p, left, top, wide, high) && inside(q, left, top, wide, high) && inside(r, left, top, wide, high))) break;
                const ImU32 pc = p[4], qc = q[4], rc = r[4];
                if ((pc != colour && pc != other) || (qc != colour && qc != other) || (rc != colour && rc != other)) break;
                to[0] = p[0], to[1] = p[1], to[2] = 0, to[3] = texels(p[2]) | texels(p[3]) << 12 | (pc == colour ? tag : other_tag);
                to[4] = q[0], to[5] = q[1], to[6] = 0, to[7] = texels(q[2]) | texels(q[3]) << 12 | (qc == colour ? tag : other_tag);
                to[8] = r[0], to[9] = r[1], to[10] = 0, to[11] = texels(r[2]) | texels(r[3]) << 12 | (rc == colour ? tag : other_tag);
            }
        }
        out.to = to;
        if (!(index + 2 < end && to <= last)) {
            // (a worker out of room: the first core makes the frame)
            if (index + 2 < end && ahead) failed = true;
            return true;
        }
        if (again) continue;
        int indices = one_shape(clip, base, index, end);
        if (!indices && !full_table()) return false;    // (with a new table, the same one again)
        index += indices;
    }
}

// The pieces from `pieces` to `pieces_end`, a command of the device's for every one that drew.
void copier::copy_pieces()
{
    run_count = 0, failed = false;
    if (!new_table()) return;
    out.quads = 0, out.next_lone = nullptr;
    more_room();
    // (a run of the device's for every texture: the clip rectangles are the copy's)
    out.name = pieces != pieces_end ? pieces->name : 0;
    for (const piece* p = pieces; p != pieces_end && out.to && !failed; p++) {
        if (p->name != out.name) flush(out.quads), out.name = p->name;
        out.records = p->records;
        if (!copy_command(p->clip, p->base, p->index, p->end)) return;
    }
    if (out.to && !failed) flush(out.quads);
}

#define MOST_AHEAD 6        // workers asked for
static copier own;          // the first core's
static scratch later_lists[MOST_AHEAD + 1];     // the workers' and (last) the first core's
static bool summing;        // IMGUI_SUM=1: an imsum: line a frame, to compare two ways of copying by
static unsigned summed;

// The triangles of some runs in order, leaving out those with two corners the same, each
// corner with its colour in place of its tag: the same whoever copied, and however cut.
static void sum_runs(const run* runs, int count, unsigned* sum, unsigned* triangles)
{
    for (int k = 0; k < count; k++) {
        const run& r = runs[k];
        for (int i = 0; i + (r.quads ? 3 : 2) < r.count; i += r.quads ? 4 : 3) {
            for (int half = 0; half < (r.quads ? 2 : 1); half++) {
                const ImU32* v[3] = {r.first + 4 * i, r.first + 4 * (i + 1 + half), r.first + 4 * (i + 2 + half)};
                bool flat = false;
                for (int a = 0; a < 3; a++) {
                    const ImU32 *p = v[a], *q = v[(a + 1) % 3];
                    flat |= p[0] == q[0] && p[1] == q[1] && p[3] == q[3];
                }
                if (flat) continue;
                for (int a = 0; a < 3; a++)
                    *sum = (((*sum * 31 + v[a][0]) * 31 + v[a][1]) * 31 + (v[a][3] & 0xffffff)) * 31 + r.table[v[a][3] >> 24];
                ++*triangles;
            }
        }
    }
}

// The last frame's runs as the device has them, kept for ImGui_ImplShaderEmu_RenderAgain(): their tables and corners
// in one block of words, each run's place in it. The block is GPU memory of its own, one of two in turn (a list still
// drawn may read the other), drawn where it lies; a frame larger than that is kept in the program's memory and copied
// into each frame.
struct kept_run { int vertices, table, count, quads; GLuint name; };
#define KEPT_BYTES (96u << 10)
static bool keeping;
static struct { ImVector<ImU32> words; ImVector<kept_run> runs; ImU32* gpu[2]; ImU32* at; int size, turn; bool whole; } kept;

void ImGui_ImplShaderEmu_KeepFrames(bool keep)
{
    keeping = keep, kept.whole = false;
    for (int i = 0; keep && i < 2; i++)
        if (!kept.gpu[i] && seglMemoryLeft() >= KEPT_BYTES + (256u << 10)) kept.gpu[i] = (ImU32*)seglMemory(KEPT_BYTES);
}

static void keep_runs()
{
    kept.runs.resize(0);
    kept.whole = !own.failed && own.run_count < MOST_RUNS;
    // (the words it takes: a table of 256 for each table, four a corner)
    int words = 0;
    const ImU32* table = nullptr;
    for (int i = 0; i < own.run_count; i++) {
        if (own.runs[i].table != table) table = own.runs[i].table, words += 256;
        words += 4 * own.runs[i].count;
    }
    kept.turn ^= 1;
    if (kept.gpu[0] && kept.gpu[1] && (unsigned)words * 4 <= KEPT_BYTES) kept.at = kept.gpu[kept.turn];
    else kept.words.resize(words), kept.at = kept.words.Data;
    kept.size = words;
    table = nullptr;
    int at = 0, table_at = 0;
    for (int i = 0; kept.whole && i < own.run_count; i++) {
        const run& r = own.runs[i];
        if (r.table != table) {
            table = r.table, table_at = at;
            memcpy(kept.at + at, table, 256 * 4), at += 256;
        }
        memcpy(kept.at + at, r.first, (size_t)r.count * 16);
        kept.runs.push_back({at, table_at, r.count, r.quads, r.name});
        at += 4 * r.count;
    }
}

bool ImGui_ImplShaderEmu_RenderAgain()
{
    if (!kept.whole) return false;
    if (!kept.runs.Size) return true;
    ImU32* from = kept.at;
    if (from == kept.words.Data) {
        // (not GPU memory of its own: a copy in the frame)
        const unsigned bytes = (unsigned)kept.size * 4;
        from = bytes <= seglFrameLeft() ? (ImU32*)seglFrameSpace(bytes) : nullptr;
        if (!from) return false;
        memcpy(from, kept.words.Data, bytes);
    }
    for (const kept_run& r : kept.runs)
        seglScreenCompactAt((const unsigned int*)(from + r.vertices), r.count, r.quads, r.name, (const unsigned int*)(from + r.table));
    return true;
}

void ImGui_ImplShaderEmu_RenderDrawData(ImDrawData* draw_data)
{
    static_assert(sizeof(ImDrawVert) == sizeof(corner) && sizeof(ImDrawIdx) == 2, "a corner is an ImDrawVert");
    static int asked;
    if (!asked) asked = 1, summing = getenv("IMGUI_SUM") != nullptr;
    own.noted = summing || keeping, own.run_count = 0, own.failed = false;
    kept.whole = false;
    lines_last.list = nullptr;
    if (deferring && draw_data->CmdListsCount) {
        own.later = &later_lists[MOST_AHEAD];
        scratch_ready(own.later, draw_data->CmdLists[0]->_Data);
    }
    if (!own.new_table()) return;
    own.out.quads = 0, own.out.next_lone = nullptr;
    own.more_room();
    bool open = false;
    for (int n = 0; n < draw_data->CmdListsCount && own.out.to; n++) {
        const ImDrawList* list = draw_data->CmdLists[n];
        own.out.records = list->_Deferred.Data;
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                // (what the program draws there comes after what is before it)
                if (open) own.flush(own.out.quads), open = false;
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                continue;
            }
            // a command of the device's for every texture: the clip rectangles are the copy's
            if (open && (GLuint)cmd.TextureId != own.out.name) own.flush(own.out.quads);
            own.out.name = (GLuint)cmd.TextureId, open = true;
            const ImDrawIdx* index = list->IdxBuffer.Data + cmd.IdxOffset;
            if (!own.copy_command(cmd.ClipRect, (const char*)(list->VtxBuffer.Data + cmd.VtxOffset), index, index + cmd.ElemCount)) return;
        }
    }
    if (open && own.out.to) own.flush(own.out.quads);
    if (keeping && own.out.to) keep_runs();
    if (summing) {
        unsigned sum = 0, triangles = 0;
        sum_runs(own.runs, own.run_count, &sum, &triangles);
        fprintf(stderr, "imsum: frame %u, %u triangles, sum %08x\n", ++summed, triangles, sum);
    }
}

// ---- the copy on worker cores (docs/apps/imgui.md) ----

#define MOST_PIECES 512
#define MOST_LISTS 32
#define AHEAD_TABLES 2      // a worker's colour tables: 512 colours in its share, or the first core copies
#define AHEAD_LEAST 1200    // indices a frame (those its records stand for too) from which the workers are worth their two passes

extern "C" char __DATA_BEGIN__[], _end[], _start[], etext[];
static copier ahead[MOST_AHEAD];
static piece pieces[MOST_PIECES];
// ImGui's vertices, indices and records of the frame the workers are copying: its lists have
// others meanwhile, these of two frames ago, which a list empties before it writes
static ImVector<ImDrawVert> kept_vertices[MOST_LISTS];
static ImVector<ImDrawIdx> kept_indices[MOST_LISTS];
static ImVector<unsigned int> kept_records[MOST_LISTS];
static int workers = -1, jobs, piece_count, touched;
static int weights[MOST_PIECES], corners[MOST_PIECES];   // each command's: its work, and the corners its records may make

static struct alignas(16) { unsigned n; } shares[MOST_AHEAD];     // IMGUI_SHARES=1: each worker's instructions (a texel each), a line every 30 frames
static bool telling;

static unsigned count_now()
{
    unsigned n;
    __asm__ volatile("rdcycle %0" : "=r"(n));
    return n;
}

static unsigned copy_job(unsigned copier_at, unsigned k)
{
    const unsigned from = telling ? count_now() : 0;
    ((copier*)copier_at)->copy_pieces();
    if (telling) shares[k].n = count_now() - from;
    return 0;
}

bool ImGui_ImplShaderEmu_RenderAhead(ImDrawData* draw_data)
{
    if (workers < 0) {
        const char* how = getenv("IMGUI_COPY");
        summing = getenv("IMGUI_SUM") != nullptr;
        telling = getenv("IMGUI_SHARES") != nullptr;
        workers = how && !strcmp(how, "inline") ? 0 : mcw_open(MOST_AHEAD);
        if (workers > MOST_AHEAD) workers = MOST_AHEAD;
        if (workers > 0) {
            atexit(mcw_close);
            // (a worker stops at a page of the program's that nobody has touched, and at a page of
            // code no core has run, the library's that the first core no longer runs among them: mcw.h)
            mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
            mcw_touch_code(_start, (unsigned)(etext - _start));
            // the lists' text, lines and filled shapes made by the workers from the next frame on
            // ("Drawn later"; IMGUI_LATER=0: by the first core as they are asked for)
            const char* later = getenv("IMGUI_LATER");
            if (!later || strcmp(later, "0")) {
                GImDrawListDefer.Text = defer_text, GImDrawListDefer.Polyline = defer_line, GImDrawListDefer.ConvexPolyFilled = defer_fill;
                GImDrawListDefer.Line = defer_segment, GImDrawListDefer.PlotLines = defer_plot;
                deferring = true;
            }
        }
    }
    lines_last.list = nullptr;
    if (workers <= 0 || jobs || draw_data->CmdListsCount > MOST_LISTS) return false;

    // every command's weight, and all of them
    int total = 0, command_count = 0;
    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* list = draw_data->CmdLists[n];
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            // (a call of the program's is the first core's to make, in its place among the draws)
            if (cmd.UserCallback && cmd.UserCallback != ImDrawCallback_ResetRenderState) return false;
            if (cmd.UserCallback) continue;
            if (command_count == MOST_PIECES) return false;
            // (a command's work: an index copied is one, and its records' as the hooks said, in UserCallbackData)
            const int w = (int)cmd.ElemCount + (int)(intptr_t)cmd.UserCallbackData;
            corners[command_count] = cmd.UserCallbackDataSize, weights[command_count++] = w, total += w;
        }
    }
    if (total < AHEAD_LEAST) return false;

    // the commands as pieces, each worker's share of the weight a run of them: a command is
    // cut where a share ends, at a whole triangle and never between a quad's two
    const int share = total / workers + 1;
    int first[MOST_AHEAD + 1], indices[MOST_AHEAD] = {0}, made[MOST_AHEAD] = {0}, job = 0, room = share, c = 0;
    piece_count = first[0] = 0;
    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* list = draw_data->CmdLists[n];
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const char* base = (const char*)(list->VtxBuffer.Data + cmd.VtxOffset);
            const ImDrawIdx *index = list->IdxBuffer.Data + cmd.IdxOffset, *end = index + cmd.ElemCount;
            int left = weights[c], left_made = corners[c++];
            while (index < end) {
                if (piece_count == MOST_PIECES) return false;
                if (room <= 0 && job + 1 < workers) first[++job] = piece_count, room = share;
                const ImDrawIdx* to = end;
                int w = left, k = left_made;
                if (left > room && job + 1 < workers) {
                    // (where the share ends in it)
                    for (to = index, w = 0, k = 0; to < end && w < room; to += 3) {
                        w += 3;
                        if (to[0] == to[1]) {
                            const ImU32* mark = (const ImU32*)(base + to[0] * sizeof(ImDrawVert));
                            k += (int)mark[2], w += (int)mark[3];
                        }
                    }
                    if (to + 2 < end && to[0] == to[-3] && to[1] == to[-1]) to += 3, w += 3;
                }
                pieces[piece_count++] = {cmd.ClipRect, (GLuint)cmd.TextureId, base, index, to, list->_Deferred.Data};
                indices[job] += (int)(to - index), made[job] += k;
                room -= w, left -= w, left_made -= k;
                index = to;
            }
        }
    }
    first[job + 1] = piece_count;

    // room for each: no cut triangle is more than five corners an index, and a quad is fewer;
    // a record's corners at most its indices, but for a few cut (or the first core makes it all)
    unsigned bytes = 0, at = 0;
    for (int k = 0; k <= job; k++) bytes += AHEAD_TABLES * 1024 + (5 * indices[k] + made[k] + made[k] / 8 + 96) * 16;
    ImU32* memory = bytes <= seglFrameLeft() ? (ImU32*)seglFrameSpace(bytes) : nullptr;
    if (!memory) return false;
    // (the frame's pages there before a worker steps on them: a set's are new to its first frames)
    if (touched < 4) {
        for (unsigned i = 0; i < bytes; i += 4096) memory[i / 4] = 0;
        touched++;
    }
    for (int k = 0; k <= job; k++) {
        copier& c = ahead[k];
        c.ahead = true;
        c.tables = memory + at, c.tables_left = AHEAD_TABLES;
        c.room = c.tables + AHEAD_TABLES * 256;
        c.room_end = c.room + (5 * indices[k] + made[k] + made[k] / 8 + 96) * 4;
        at += (unsigned)(c.room_end - c.tables);
        c.pieces = pieces + first[k], c.pieces_end = pieces + first[k + 1];
        if (deferring) c.later = &later_lists[k], scratch_ready(c.later, draw_data->CmdLists[0]->_Data);
    }
    if (deferring) own.later = &later_lists[MOST_AHEAD], scratch_ready(own.later, draw_data->CmdLists[0]->_Data);
    // the lists' vertices, indices and records are ours until the frame is finished
    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        draw_data->CmdLists[n]->VtxBuffer.swap(kept_vertices[n]);
        draw_data->CmdLists[n]->IdxBuffer.swap(kept_indices[n]);
        draw_data->CmdLists[n]->_Deferred.swap(kept_records[n]);
    }
    jobs = job + 1;
    for (int k = 0; k < jobs; k++) mcw_post(k + 1, copy_job, (unsigned)&ahead[k], (unsigned)k);
    return true;
}

bool ImGui_ImplShaderEmu_RenderReady()
{
    for (int k = 0; k < jobs; k++)
        if (!mcw_done(k + 1)) return false;
    return true;
}

void ImGui_ImplShaderEmu_RenderFinish()
{
    if (!jobs) return;
    bool failed = false;
    for (int k = 0; k < jobs; k++) {
        mcw_wait(k + 1);
        failed |= ahead[k].failed;
    }
    unsigned sum = 0, triangles = 0;
    if (failed) {
        // (colours or runs beyond what a worker has room for: all of it again, by this core)
        own.ahead = false, own.noted = summing;
        own.pieces = pieces, own.pieces_end = pieces + piece_count;
        own.copy_pieces();
        if (summing) sum_runs(own.runs, own.run_count, &sum, &triangles);
    } else {
        for (int k = 0; k < jobs; k++) {
            const copier& c = ahead[k];
            for (int i = 0; i < c.run_count; i++)
                seglScreenCompactAt((const unsigned int*)c.runs[i].first, c.runs[i].count, c.runs[i].quads, c.runs[i].name,
                                    (const unsigned int*)c.runs[i].table);
            if (summing) sum_runs(c.runs, c.run_count, &sum, &triangles);
        }
    }
    if (summing) fprintf(stderr, "imsum: frame %u, %u triangles, sum %08x%s\n", ++summed, triangles, sum, failed ? " (this core's)" : "");
    static unsigned finished;
    if (telling && ++finished % 30 == 0) {
        fprintf(stderr, "imshares: %d of %d workers%s:", jobs, workers, failed ? " (failed: this core's)" : "");
        for (int k = 0; k < jobs; k++) fprintf(stderr, " %u", shares[k].n);
        fprintf(stderr, "\n");
    }
    jobs = 0;
}
