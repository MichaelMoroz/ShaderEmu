// Tiberian Dawn's map drawn by the machine's GPU (docs/games/tdawn.md). The terrain is quads that
// stay, one a cell; overlays, objects and the shroud are quads of each frame, from atlases
// their pictures are put into the first time they are drawn. The game's own page goes on top.
#include "function.h"
#include "common/host.h"
#include "common/keyframe.h"
#include "common/mcwx.h" // (mcw_self: which core draws)

enum
{
    ATLAS_W = 1024,
    ATLAS_ROWS = 2048,
    ICON_ROWS = 1008, // the atlas's first rows hold terrain icons, each in a square of 26
    ICON_ACROSS = 39,
    ICON_MOST = 39 * 38,
    SHADE_ACROSS = 9,
    SHADE_MOST = 9 * 6, // the shroud's shapes, in the shades, each in a square of 26
    SHADE_ROW = 256,
    KEY = 0,             // the page's index that lets the scene through: the game never draws it
    SLOTS = 4096,
    TILES = 64 * 64, // kept quads: the cells of a map no larger than the original game's
};

#define SHAPE_TRANS 0x40 // conquer.cpp's and keybuff.cpp's: index 0 is not drawn

// keybuff.cpp's: what Build_Frame returns when shapes are kept uncompressed
struct ShapeHeader
{
    unsigned draw_flags;
    char* shape_data;
    int shape_buffer;
};

// stamp.cpp's: the icon set it looked at last
struct IconControlType;
void Init_Stamps(const IconControlType* iconset);
extern const uint8_t* StampPtr;
extern const uint8_t* MapPtr;
extern int IconCount;

enum { NEW, KEYED, MASKED, SHADE, EMPTY, FAILED };

struct Shape
{
    const void* file;
    const void* fade;
    const void* ghost;
    unsigned short frame;
    short x, y, w, h;     // where it is in its atlas: only the part of it that has pixels
    short left, top;      // where that part is in the whole shape
    short full_w, full_h; // and how large that is
    unsigned char kind;
};

struct Icon
{
    const void* set;
    short icon, index;
};

bool ShaderEmu_GL_Live;           // the GPU draws the map: a changed cell does not have its objects drawn again
static int box[4] = {1, 1, 0, 0}; // or only inside this rectangle of the page (x0, y0, x1, y1)

// An object's rectangles as it drew them last, by their place on the map: drawn again as they
// are while the game has not marked the object as changed.
typedef struct host_quad Quad; // (the scene takes an object's kept ones as they lie: host_quads)
// TechnoClass::Draw_It's bars and box, its place by the map's pixels (ShaderEmu_GL_Bars)
struct BarsDraw
{
    const unsigned char* shade;
    short x, y, pwidth;
    unsigned char lx, ly, dx, dy, fudge, width, height, color;
    bool selected;
};
struct Kept
{
    const ObjectClass* who;
    unsigned char count;
    bool whole; // these are all of it: nothing the game drew itself, and no more than fit
    bool away;  // or it drew nothing, being out of the view when the view was at...
    COORDINATE view;
    Quad quad[6];
    bool barred; // with bars, put into the page every frame it is shown
    BarsDraw bars;
};
enum { KEPT = 1024 };
alignas(16) static Kept kept[KEPT];
static int kept_count;
// What the core that draws the objects stores outside its own memory (a worker, while the logic
// goes on: "behind" below), in texels of its own: two cores must not store to one 16 bytes.
static struct alignas(16) Drawing
{
    Kept* recording;
    unsigned quads, shroud, shadows, objects[3], put[2], refused;
    int shelf_x[33], shelf_y[33], shelf_top; // a row of the atlas for each height, in 16s
    unsigned frames_taken, frames_differ;
    int check_frames;
    bool emitting, touched;
    bool missed, not_here; // (the worker: a shape it could not draw, and of those one the game would have)
    bool full;             // no room in the atlas for a shape: its shapes are forgotten at the next scene
    unsigned misses;
} drawing = {NULL, 0, 0, 0, {0, 0, 0}, {0, 0}, 0, {0}, {0}, 0, 0, 0, -1, false, true, false, false, false, 0};
#define recording drawing.recording
#define shelf_x drawing.shelf_x
#define shelf_y drawing.shelf_y
#define shelf_top drawing.shelf_top
#define frames_taken drawing.frames_taken
#define frames_differ drawing.frames_differ
#define check_frames drawing.check_frames
bool& ShaderEmu_GL_Emitting = drawing.emitting; // the map's shapes and stamps go to the GPU
bool& ShaderEmu_GL_Touched = drawing.touched;   // the game drew over the map itself: the page has more than holes there
unsigned& ShaderEmu_GL_Quads = drawing.quads;   // all of them, and of those the shroud's and the shadows'
unsigned& ShaderEmu_GL_Shroud = drawing.shroud;
unsigned& ShaderEmu_GL_Shadows = drawing.shadows;
unsigned (&ShaderEmu_GL_Objects)[3] = drawing.objects; // shown as they were, found out of the view again, drawn anew
unsigned (&ShaderEmu_GL_Put)[2] = drawing.put;         // shapes put into an atlas, and the pixels of their frames
static int origin_x, origin_y, view_w, view_h; // where the map's corner is on the screen, and the view's size
unsigned ShaderEmu_GL_Parts[3]; // instructions in the cells, the objects and the shroud
// The bars and boxes put into the page (the drawing core's, by the page's pixels): made holes
// again before the next scene. Any other drawing over the map makes the whole page holes.
enum { BARS = 96 };
static struct alignas(16) Bars
{
    int count;
    BarsDraw drawn[BARS];
} bars;
static int message_rows; // the rows of the view's top the messages take

alignas(16) static Shape shapes[SLOTS];
static Icon icons[SLOTS];
static int icon_count, shade_count;
static unsigned char tile_done[TILES / 8];
static signed char edge_of[TILES]; // the shroud's shape over a cell, as last worked out (-2: not yet)
static Shape* edge_shape[64];      // and where each of those shapes is in the shades
static int tile_x, tile_y, tile_w, tile_h; // the map's cells that have a quad
static int shown_x0, shown_y0, shown_x1, shown_y1; // the cells of the view drawn last
bool ShaderEmu_GL_Stale; // something in that view changed since (map.cpp looks before it tells of more)
#define stale ShaderEmu_GL_Stale
// What is under the objects and over them (the cells' own rectangles, the shroud) stays in the
// scene as it was noted (host_scene_part) while no cell of the view has changed.
static bool under_stale = true;
static int under_view[4];
static const unsigned char* shroud_table;
static const unsigned char* special_ghost;
static int solid_x, solid_y; // 24 x 24 set bits in the masks: a cell not seen yet
static char scenario[16];

// TDAWN_RENDER=soft: the game draws the map itself, as it does where there is no room for this.
static bool available(void)
{
    static int state = -1;

    if (state < 0) {
        const char* how = getenv("TDAWN_RENDER");
        state = !(how && !strcmp(how, "soft")) && host_scene_open(TILES) == 0;
    }
    return state > 0;
}

// A cell's quad, or -1 for one outside the map.
static int tile_of(CELL cell)
{
    int x = Cell_X(cell) - tile_x, y = Cell_Y(cell) - tile_y;

    return x >= 0 && y >= 0 && x < tile_w && y < tile_h ? y * tile_w + x : -1;
}

// Shape files asked for ahead of their first use (host.h), as a mission begins: what is on the
// map, and what the game told of (ShaderEmu_GL_Ahead_File: the sidebar's pieces, which it draws
// itself, all in the frame the sidebar comes). Called every frame, before its logic.
static struct
{
    const void* file;
    int frame;
} ahead_files[96];
static int ahead_count;
static bool ahead_done; // the frames made ready are this mission's: forget() leaves them
static bool ahead_live; // a mission has begun: a file told of from now on is asked for at once
void ShaderEmu_GL_Ahead_File(const void* file, int frame)
{
    for (int i = 0; i < ahead_count; i++) {
        if (ahead_files[i].file == file) {
            return;
        }
    }
    if (file && ahead_count < 96) {
        ahead_files[ahead_count].file = file;
        ahead_files[ahead_count++].frame = frame;
        if (ahead_live) {
            host_frames_ahead(file, frame);
        }
    }
}

// Everything there is of the kinds that are drawn, on the map or not yet (being built, on its
// way in a transport): its file is unpacked before it is first seen.
static void ahead_objects(void)
{
    for (int i = 0; i < Units.Count(); i++) {
        host_frames_ahead(Units.Ptr(i)->Class_Of().Get_Image_Data(), 0);
    }
    for (int i = 0; i < Infantry.Count(); i++) {
        host_frames_ahead(Infantry.Ptr(i)->Class_Of().Get_Image_Data(), 0);
    }
    for (int i = 0; i < Aircraft.Count(); i++) {
        host_frames_ahead(Aircraft.Ptr(i)->Class_Of().Get_Image_Data(), 0);
    }
    for (int i = 0; i < Buildings.Count(); i++) {
        host_frames_ahead(Buildings.Ptr(i)->Class_Of().Get_Image_Data(), 0);
        host_frames_ahead(Buildings.Ptr(i)->Class->Get_Buildup_Data(), 0);
    }
}

void ShaderEmu_GL_Ahead(void)
{
    static char name[16];
    static int last = -1, objects;
    bool begins = strncmp(name, Scen.ScenarioName, sizeof(name) - 1) || (int)Frame < last;
    int now = Units.Count() + Infantry.Count() + Buildings.Count() + Aircraft.Count();

    last = (int)Frame;
    if (!begins) {
        if (now != objects && ahead_live) {
            objects = now;
            ahead_objects();
        }
        return;
    }
    if (!available()) {
        return;
    }
    objects = now;
    strncpy(name, Scen.ScenarioName, sizeof(name) - 1);
    host_frames_forget();
    for (LayerType layer = LAYER_GROUND; layer < LAYER_COUNT; layer++) {
        for (int index = 0; index < Map.Layer[layer].Count(); index++) {
            host_frames_ahead(Map.Layer[layer][index]->Class_Of().Get_Image_Data(), 0);
        }
    }
    ahead_objects();
    for (int i = 0; i < ahead_count; i++) {
        host_frames_ahead(ahead_files[i].file, ahead_files[i].frame);
    }
    for (int i = ANIM_FIRST; i < ANIM_COUNT; i++) {
        host_frames_ahead(AnimTypeClass::As_Reference((AnimType)i).Get_Image_Data(), 0);
    }
    ahead_done = ahead_live = true;
}

static void forget(void)
{
    if (ahead_done) {
        host_frames_settle(); // (first: work put off for the scene's core still looks at the shapes)
    } else {
        host_frames_forget();
    }
    ahead_done = false;
    memset(shapes, 0, sizeof(shapes));
    memset(icons, 0, sizeof(icons));
    memset(tile_done, 0, sizeof(tile_done));
    memset(shelf_x, 0, sizeof(shelf_x));
    memset(shelf_y, 0, sizeof(shelf_y));
    icon_count = shade_count = 0;
    shelf_top = ICON_ROWS;
    host_tiles_clear();
    memset(host_masks(), 0, ATLAS_W / 8 * ATLAS_ROWS);
    memset(kept, 0, sizeof(kept));
    memset(edge_of, -2, sizeof(edge_of));
    memset(edge_shape, 0, sizeof(edge_shape));
    kept_count = 0;
    solid_x = -1;
}

// The atlas full: its shapes are forgotten and put again as they are next drawn (docs/games/tdawn.md,
// "A long battle"). In a long battle every new shape was the game's own drawing once it had filled.
static unsigned shapes_forgotten, forget_every;
static void shapes_forget(void)
{
    host_frames_settle(); // (work put off for the scene's core still looks at the shapes)
    host_settle();        // (and the list the GPU may still draw)
    memset(shapes, 0, sizeof(shapes));
    memset(shelf_x, 0, sizeof(shelf_x));
    memset(shelf_y, 0, sizeof(shelf_y));
    shade_count = 0;
    shelf_top = ICON_ROWS;
    memset(host_masks() + ICON_ROWS * (ATLAS_W / 8), 0, (ATLAS_ROWS - ICON_ROWS) * (ATLAS_W / 8));
    memset(kept, 0, sizeof(kept));
    memset(edge_of, -2, sizeof(edge_of));
    memset(edge_shape, 0, sizeof(edge_shape));
    kept_count = 0;
    solid_x = -1;
    under_stale = true;
    drawing.full = false;
    shapes_forgotten++;
}

// For a test (TDAWN_CHECK_REDRAW): the next scene is made from nothing, the atlas too, and its
// shapes from the game's own frames.
static bool check_plain;
void ShaderEmu_GL_Anew(void)
{
    if (ShaderEmu_GL_Live) {
        host_scene_settle();
        forget();
        check_plain = true;
    }
}

// Something of this cell changed (CellClass::Redraw_Objects): its edge of the shroud is worked
// out again, and the scene made again if the cell is in it.
void ShaderEmu_GL_Cell(CELL cell)
{
    int x = Cell_X(cell), y = Cell_Y(cell), tx = x - tile_x, ty = y - tile_y;

    if (tx >= 0 && ty >= 0 && tx < tile_w && ty < tile_h) {
        edge_of[ty * tile_w + tx] = -2;
    }
    if (x >= shown_x0 && x <= shown_x1 && y >= shown_y0 && y <= shown_y1) {
        under_stale = true;
        if (!stale) {
            stale = true;
            Map.Flag_To_Redraw(false);
        }
    }
}

// An object here changed (DisplayClass::Refresh_Cells): the scene is made again if that is near it.
void ShaderEmu_GL_Near(CELL cell)
{
    int x = Cell_X(cell), y = Cell_Y(cell);

    if (!stale && x >= shown_x0 - 5 && x <= shown_x1 + 5 && y >= shown_y0 - 5 && y <= shown_y1 + 5) {
        stale = true;
        Map.Flag_To_Redraw(false);
    }
}

// Whether an object that is to be drawn again is in the scene shown, or within five cells of it.
bool ShaderEmu_GL_Changed(void)
{
    if (!ShaderEmu_GL_Live) {
        return false;
    }
    static uintptr_t before;
    uintptr_t all = 0; // which objects there are: one that has gone says nothing itself
    bool near = false;
    for (LayerType layer = LAYER_GROUND; layer < LAYER_COUNT; layer++) {
        int count = Map.Layer[layer].Count();
        ObjectClass* const* list = count ? &Map.Layer[layer][0] : NULL;
        for (int index = 0; index < count; index++) {
            const ObjectClass* object = list[index];
            all += (uintptr_t)object; // (whatever their order: an object lies where no other lies)
            if (object->IsToDisplay && !near) {
                int x = Coord_XCell(object->Coord), y = Coord_YCell(object->Coord);
                near = x >= shown_x0 - 5 && x <= shown_x1 + 5 && y >= shown_y0 - 5 && y <= shown_y1 + 5;
            }
        }
    }
    near |= stale || all != before;
    before = all;
    return near;
}

// The messages over the map stay in the page as they were printed: whether they are to be
// printed again, which is when they are others (their place is made holes first) or when
// holes were made in the page since. A message was printed anew every frame it was up, 14k.
static unsigned fills;
bool ShaderEmu_GL_Messages(unsigned signature, int count)
{
    static unsigned last_signature, last_fills;
    static int last_count;

    if (signature == last_signature && fills == last_fills) {
        return false;
    }
    if (signature != last_signature && MAX(count, last_count) > 0) {
        int wide = Lepton_To_Pixel(Map.TacLeptonWidth), high = Lepton_To_Pixel(Map.TacLeptonHeight);
        int rows = MIN(10 * MAX(count, last_count) + 10, high - 1);
        LogicPage->Fill_Rect(Map.TacPixelX, Map.TacPixelY, Map.TacPixelX + wide - 1, Map.TacPixelY + rows, KEY);
    }
    last_signature = signature;
    last_fills = fills;
    last_count = count;
    message_rows = count > 0 ? 10 * count + 10 : 0;
    return true;
}

// The page's pixels inside a rectangle (x0, y0, x1, y1), its far edges in it, as the game's own
// drawing clips them to a window.
struct Pen
{
    unsigned char* base;
    int stride, x0, y0, x1, y1;
};

static void pen_row(const Pen& p, int xa, int xb, int y, int colour)
{
    if (xa > xb) {
        int t = xa;
        xa = xb, xb = t;
    }
    xa = MAX(xa, p.x0), xb = MIN(xb, p.x1);
    if (y >= p.y0 && y <= p.y1 && xa <= xb) {
        memset(p.base + y * p.stride + xa, colour, xb - xa + 1);
    }
}

static void pen_column(const Pen& p, int x, int ya, int yb, int colour)
{
    if (ya > yb) {
        int t = ya;
        ya = yb, yb = t;
    }
    ya = MAX(ya, p.y0), yb = MIN(yb, p.y1);
    for (int y = ya; x >= p.x0 && x <= p.x1 && y <= yb; y++) {
        p.base[y * p.stride + x] = (unsigned char)colour;
    }
}

// Bars and box at (x, y) of the page as Draw_It's calls draw them (Remap, Draw_Rect, Fill_Rect,
// Draw_Line), or, with a key, those pixels made that.
static void bars_pen(const Pen& p, const BarsDraw& b, int x, int y, int key)
{
    int lx = b.lx, ly = b.ly, dx = b.dx, dy = b.dy, white = key < 0 ? WHITE : key;
    if (b.pwidth >= 0) {
        int xx = x - b.width / 2, yy = y - b.height / 2, w = b.width;
        if (key >= 0) {
            for (int row = yy; row <= yy + 3; row++) {
                pen_row(p, xx, xx + w - 1, row, key);
            }
        } else {
            for (int row = MAX(yy + 1, p.y0); row <= MIN(yy + 2, p.y1); row++) {
                unsigned char* at = p.base + row * p.stride;
                for (int col = MAX(xx + 1, p.x0); col <= MIN(xx + w - 1, p.x1); col++) {
                    at[col] = b.shade[at[col]];
                }
            }
            pen_row(p, xx, xx + w - 1, yy, BLACK);
            pen_row(p, xx, xx + w - 1, yy + 3, BLACK);
            pen_column(p, xx, yy, yy + 3, BLACK);
            pen_column(p, xx + w - 1, yy, yy + 3, BLACK);
            for (int row = yy + 1; b.pwidth >= 1 && row <= yy + 2; row++) {
                pen_row(p, xx + 1, xx + b.pwidth, row, b.color);
            }
        }
    }
    if (b.selected) {
        int top = b.fudge + y - ly;
        pen_row(p, x - lx, x - lx + dx, top, white);
        pen_column(p, x - lx, top, top + dy, white);
        pen_row(p, x + lx, x + lx - dx, top, white);
        pen_column(p, x + lx, top, top + dy, white);
        pen_row(p, x + lx, x + lx - dx, y + ly, white);
        pen_column(p, x + lx, y + ly, y + ly - dy, white);
        pen_row(p, x - lx, x - lx + dx, y + ly, white);
        pen_column(p, x - lx, y + ly, y + ly - dy, white);
    }
}

// An object's bars and box from the map's pixels to the page, noted for the next scene.
static void bars_put(const BarsDraw& b)
{
    int wx = WindowList[WINDOW_TACTICAL][WINDOWX], wy = WindowList[WINDOW_TACTICAL][WINDOWY];
    // (the hidden page's pixels, not its Lock: the lock's count is the first core's to store to)
    unsigned char* page = host_page(1);
    if (!page || LogicPage != &HidPage) {
        ShaderEmu_GL_Touched = true; // (not the page the scene is under: all of it holes next time)
        return;
    }
    host_settle();
    Pen p = {page, LogicPage->Get_Width() + LogicPage->Get_XAdd() + LogicPage->Get_Pitch(),
             wx, wy, wx + WindowList[WINDOW_TACTICAL][WINDOWWIDTH] - 1, wy + WindowList[WINDOW_TACTICAL][WINDOWHEIGHT] - 1};
    bars_pen(p, b, origin_x + b.x, origin_y + b.y, -1);
    if (bars.count == BARS) {
        ShaderEmu_GL_Touched = true; // (more than there is room for: the page is made all holes)
        return;
    }
    BarsDraw& to = bars.drawn[bars.count++];
    to = b;
    to.x = (short)(origin_x + b.x), to.y = (short)(origin_y + b.y);
}

// TechnoClass::Draw_It's bars and box (x, y in the tactical window), drawn and kept with the
// object being drawn. False: the game is to draw them itself.
bool ShaderEmu_GL_Bars(int x, int y, int lx, int ly, int dx, int dy, int fudge, int width, int height, int pwidth,
                       int color, bool selected, const void* shade)
{
    Kept* k = recording;
    if (!ShaderEmu_GL_Emitting || !k || LogicPage != &HidPage || !host_page(1) || lx < 0 || ly < 0 || dx < 0
        || dy < 0 || fudge < 0 || lx > 255 || ly > 255 || dx > 255 || dy > 255 || width < 2 || width > 255
        || height > 255) {
        return false;
    }
    BarsDraw& b = k->bars;
    b.shade = (const unsigned char*)shade;
    b.x = (short)(WindowList[WINDOW_TACTICAL][WINDOWX] + x - origin_x);
    b.y = (short)(WindowList[WINDOW_TACTICAL][WINDOWY] + y - origin_y);
    b.pwidth = (short)pwidth;
    b.lx = (unsigned char)lx, b.ly = (unsigned char)ly, b.dx = (unsigned char)dx, b.dy = (unsigned char)dy;
    b.fudge = (unsigned char)fudge, b.width = (unsigned char)width, b.height = (unsigned char)height;
    b.color = (unsigned char)color, b.selected = selected;
    k->barred = true;
    bars_put(b);
    return true;
}

// The game will draw inside this rectangle of the page itself.
void ShaderEmu_GL_Mark(int x0, int y0, int x1, int y1)
{
    if (box[0] > box[2]) {
        box[0] = x0, box[1] = y0, box[2] = x1, box[3] = y1;
    } else {
        box[0] = MIN(box[0], x0), box[1] = MIN(box[1], y0), box[2] = MAX(box[2], x1), box[3] = MAX(box[3], y1);
    }
}

// A rectangle of the scene, remembered for the object being drawn.
static void emit(int kind, int x, int y, int w, int h, int ax, int ay, unsigned colour)
{
    host_sprite(kind, x, y, w, h, ax, ay, colour);
    ShaderEmu_GL_Quads++;
    if (recording) {
        if (recording->count == 6) {
            recording->whole = false;
            return;
        }
        Quad& q = recording->quad[recording->count++];
        q.x = (short)(x - origin_x), q.y = (short)(y - origin_y), q.w = (short)w, q.h = (short)h;
        q.ax = (short)ax, q.ay = (short)ay, q.kind = (unsigned char)kind, q.colour = colour;
    }
}

static Kept* kept_of(ObjectClass* who)
{
    // (the object says where it was found last: most of them are found there every frame)
    unsigned at = (unsigned)who->ShaderEmu_Kept - 1;

    if (at < KEPT && kept[at].who == who) {
        return &kept[at];
    }
    at = (unsigned)((uintptr_t)who >> 3) % KEPT;
    for (int tries = 0; tries < 16; tries++, at = (at + 1) % KEPT) {
        if (!kept[at].who) {
            kept[at].who = who;
            kept[at].whole = kept[at].away = false;
            kept_count++;
        }
        if (kept[at].who == who) {
            who->ShaderEmu_Kept = (short)(at + 1);
            return &kept[at];
        }
    }
    return NULL;
}


// Room in the atlas, on a shelf of pictures about as high.
static bool room(int w, int h, int& x, int& y)
{
    int tall = (h + 15) / 16;

    if (w <= 0 || h <= 0 || w > ATLAS_W || tall > 32) {
        return false;
    }
    if (!shelf_y[tall] || shelf_x[tall] + w > ATLAS_W) {
        if (shelf_top + tall * 16 > ATLAS_ROWS) {
            drawing.full = true;
            return false;
        }
        shelf_y[tall] = shelf_top;
        shelf_x[tall] = 0;
        shelf_top += tall * 16;
    }
    x = shelf_x[tall];
    y = shelf_y[tall];
    shelf_x[tall] += w;
    return true;
}

// How dark a level of a translucency table makes things, as a black of that transparency:
// what the table turns white into says.
static unsigned shade_of(const unsigned char* ghost, int level)
{
    const unsigned char* to = &GamePalette[ghost[256 + level * 256 + WHITE] * 3];
    const unsigned char* white = &GamePalette[WHITE * 3];
    int before = white[0] + white[1] + white[2], after = to[0] + to[1] + to[2];

    return (unsigned)(before ? (after * 255 / before) & 0xff : 128) << 24;
}

// A ready frame's pixels into the atlas, by the core that makes the scene (host_fill): what
// put() does a pixel at a time, with the shape's place and its masks decided already.
static void fill_shape(void* shape, const void* from)
{
    const Shape* s = (const Shape*)shape;
    const unsigned char* pixels = (const unsigned char*)from;
    const unsigned char* fade = (const unsigned char*)s->fade;
    const unsigned char* ghost = (const unsigned char*)s->ghost;

    for (int y = 0; y < s->h; y++, pixels += s->w) {
        unsigned char* to = host_atlas() + (s->y + y) * ATLAS_W + s->x;
        if (!ghost && !fade) {
            memcpy(to, pixels, s->w);
            continue;
        }
        for (int x = 0; x < s->w; x++) {
            unsigned char p = pixels[x];
            if (p) {
                if (ghost && ghost[p] != 0xff) {
                    int mx = s->x, my = s->y;
                    host_masks()[(my + y) * (ATLAS_W / 8) + ((mx + x) >> 3)] |= 0x80 >> ((mx + x) & 7);
                    p = 0;
                } else if (fade) {
                    p = fade[p];
                }
            }
            to[x] = p;
        }
    }
}

// Whether a ready frame's shape could be left to that: its shadow's levels are found from the
// indexes the frame has. False (more levels than masks, no room): put() does it all, as before.
static bool deferred(Shape* s, const struct host_frame* ready)
{
    static const unsigned char lowest[32] = {0,  1,  28, 2,  29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4,  8,
                                             31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6,  11, 5,  10, 9};
    const unsigned char* ghost = (const unsigned char*)s->ghost;
    const unsigned* seen = (const unsigned*)(ready->pixels - 32);
    int count = 0;

    for (int word = 0; ghost && word < 8; word++) {
        for (unsigned bits = word ? seen[word] : seen[0] & ~1u; bits; bits &= bits - 1) {
            unsigned char level = ghost[word * 32 + lowest[((bits & (0u - bits)) * 0x077CB531u) >> 27]];
            if (level != 0xff) {
                count = 1;
            }
        }
    }
    if (count) {
        s->kind = MASKED;
    }
    host_fill(fill_shape, s, ready->pixels);
    return true;
}

static void put(Shape* s, const unsigned char* pixels, const struct host_frame* ready = NULL)
{
    const unsigned char* fade = (const unsigned char*)s->fade;
    const unsigned char* ghost = (const unsigned char*)s->ghost;
    int x, y;

    ShaderEmu_GL_Put[0]++;
    ShaderEmu_GL_Put[1] += s->w * s->h;

    if (ghost && ghost == shroud_table) {
        // the shroud: every level of dark in one picture of words
        if (s->w > 24 || s->h > 24 || shade_count >= SHADE_MOST) {
            s->kind = FAILED;
            return;
        }
        unsigned level[256];
        bool known[256] = {false};
        s->x = shade_count % SHADE_ACROSS * 26 + 1;
        s->y = shade_count / SHADE_ACROSS * 26 + 1;
        shade_count++;
        for (y = 0; y < s->h; y++) {
            unsigned* to = host_shades() + (s->y + y) * SHADE_ROW + s->x;
            for (x = 0; x < s->w; x++) {
                unsigned char p = *pixels++, g = ghost[p];
                if (!p) {
                    to[x] = 0xff000000u;
                } else if (g == 0xff) {
                    const unsigned char* c = &GamePalette[p * 3];
                    to[x] = (unsigned)(c[0] << 2) << 16 | (unsigned)(c[1] << 2) << 8 | (unsigned)(c[2] << 2);
                } else {
                    if (!known[g]) {
                        level[g] = shade_of(ghost, g);
                        known[g] = true;
                    }
                    to[x] = level[g];
                }
            }
        }
        // (its edge once more around it, as an icon's: a rectangle sampled on its edge)
        for (y = 0; y < s->h; y++) {
            unsigned* row = host_shades() + (s->y + y) * SHADE_ROW + s->x;
            row[-1] = row[0], row[s->w] = row[s->w - 1];
        }
        unsigned* top = host_shades() + s->y * SHADE_ROW + s->x - 1;
        memcpy(top - SHADE_ROW, top, (s->w + 2) * sizeof(unsigned));
        memcpy(top + s->h * SHADE_ROW, top + (s->h - 1) * SHADE_ROW, (s->w + 2) * sizeof(unsigned));
        s->kind = SHADE;
        return;
    }
    // only the rectangle that has pixels is kept: a soldier is a small figure in a large frame
    int ax, ay, x0 = s->w, x1 = -1, y0 = -1, y1 = -1, full_w = s->w;
    if (ready) {
        // (a worker core found the rectangle, and has its pixels in rows of their own)
        x0 = ready->left, y0 = ready->top, x1 = x0 + ready->w - 1, y1 = y0 + ready->h - 1;
    }
    for (y = 0; y < s->h && !ready; y++) {
        const unsigned char* row = pixels + y * full_w;
        int a = host_first_pixel(row, full_w);
        if (a == full_w) {
            continue;
        }
        int b = host_last_pixel(row, full_w);
        y0 = y0 < 0 ? y : y0;
        y1 = y;
        x0 = MIN(x0, a);
        x1 = MAX(x1, b);
    }
    if (y0 < 0) {
        s->kind = EMPTY;
        return;
    }
    // (a pixel of holes around it: a rectangle sampled on its edge, as over a window stretched
    // by a fraction, must not show the next shape of the atlas)
    if (!room(x1 - x0 + 3, y1 - y0 + 3, ax, ay)) {
        s->kind = FAILED;
        return;
    }
    for (y = 0; y < y1 - y0 + 3; y++) {
        unsigned char* row = host_atlas() + (ay + y) * ATLAS_W + ax;
        if (y == 0 || y == y1 - y0 + 2) {
            memset(row, 0, x1 - x0 + 3);
        } else {
            row[0] = row[x1 - x0 + 2] = 0;
        }
    }
    ax++, ay++;
    s->left = (short)x0;
    s->top = (short)y0;
    s->w = (short)(x1 - x0 + 1);
    s->h = (short)(y1 - y0 + 1);
    if (ready) {
        pixels = ready->pixels;
        full_w = ready->w;
    } else {
        pixels += y0 * full_w + x0;
    }
    s->x = ax;
    s->y = ay;
    s->kind = KEYED;
    if (ready && deferred(s, ready)) {
        return;
    }
    for (y = 0; y < s->h; y++, pixels += full_w) {
        unsigned char* to = host_atlas() + (ay + y) * ATLAS_W + ax;
        unsigned char* bits = host_masks() + (ay + y) * (ATLAS_W / 8);
        if (!ghost && !fade) {
            memcpy(to, pixels, s->w);
            continue;
        }
        for (x = 0; x < s->w; x++) {
            unsigned char p = pixels[x];
            if (p) {
                // a pixel the table makes see-through is the shadow: a bit of the mask (all clear so far)
                if (ghost && ghost[p] != 0xff) {
                    bits[(ax + x) >> 3] |= 0x80 >> ((ax + x) & 7);
                    s->kind = MASKED;
                    p = 0;
                } else if (fade) {
                    p = fade[p];
                }
            }
            to[x] = p;
        }
    }
}

static Shape* shape_of(const void* file, int frame, const void* fade, const void* ghost)
{
    unsigned at = ((uintptr_t)file * 31u + (unsigned)frame * 131u + (uintptr_t)fade * 7u + (uintptr_t)ghost) % SLOTS;

    for (int tries = 0; tries < 64; tries++, at = (at + 1) % SLOTS) {
        Shape* s = &shapes[at];
        if (s->kind == NEW) {
            s->file = file;
            s->frame = (unsigned short)frame;
            s->fade = fade;
            s->ghost = ghost;
            return s;
        }
        if (s->file == file && s->frame == frame && s->fade == fade && s->ghost == ghost) {
            return s;
        }
    }
    return NULL;
}

// TDAWN_CHECK_FRAMES=1: a frame a worker core made ready is built by the game too, and compared
// (0: only the count is printed when the game ends).
static void check_report(void)
{
    fprintf(stderr, "tdawn: %u frames from a worker core (%u KB of them made ready)", frames_taken, host_frames_bytes >> 10);
    if (check_frames > 1) {
        fprintf(stderr, ", %u of them differ from the game's own", frames_differ);
    }
    fputc(10, stderr);
}

static void check_frame(const Shape* s, const struct host_frame* ready, uintptr_t built)
{
    const unsigned char* pixels = (const unsigned char*)built;
    int x0 = s->w, x1 = -1, y0 = -1, y1 = -1, y;

    if (built && UseBigShapeBuffer) {
        ShapeHeader* header = (ShapeHeader*)built;
        pixels = (unsigned char*)(header->shape_buffer ? TheaterShapeBufferStart : BigShapeBufferStart) + (uintptr_t)header->shape_data;
    }
    for (y = 0; pixels && y < s->h; y++) {
        const unsigned char* row = pixels + y * s->w;
        int a = host_first_pixel(row, s->w);
        if (a < s->w) {
            int b = host_last_pixel(row, s->w);
            y0 = y0 < 0 ? y : y0, y1 = y;
            x0 = MIN(x0, a), x1 = MAX(x1, b);
        }
    }
    bool same = y0 < 0 ? ready->state == HOST_FRAME_EMPTY
                       : (ready->state & HOST_FRAME_READY) && ready->left == x0 && ready->top == y0
                             && ready->w == x1 - x0 + 1 && ready->h == y1 - y0 + 1;
    for (y = y0; same && y0 >= 0 && y <= y1; y++) {
        same = !memcmp(pixels + y * s->w + x0, ready->pixels + (y - y0) * ready->w, ready->w);
    }
    frames_differ += !same;
}

// A shape of the map, as CC_Draw_Shape was asked for it. False: the game is to draw it.
// A frame no core has ready, made by the worker that draws the objects as the workers that make
// frames ready make them (keyframe.cpp), into a buffer of its own; NULL: larger than that.
extern "C" int ShaderEmu_Frame_Into(const void* file, int frame, void* buffer);
alignas(16) static unsigned char behind_buffer[4096 + 65536]; // (4 KB before the frame: keyframe.cpp)
static const unsigned char* behind_frame(const void* file, int frame)
{
    drawing.misses++;
    if (Get_Build_Frame_Width(file) * Get_Build_Frame_Height(file) > 65536
        || !ShaderEmu_Frame_Into(file, frame, behind_buffer + 4096)) {
        return NULL;
    }
    return behind_buffer + 4096;
}

bool ShaderEmu_GL_Shape(void const* file, int frame, int x, int y, int window, int flags, void const* fade, void const* ghost)
{
    if (!ShaderEmu_GL_Emitting || window != WINDOW_TACTICAL || !file || frame == -1) {
        return false;
    }
    // (as the game does: an aircraft's or a bullet's shadow)
    if ((flags & (SHAPE_FADING | SHAPE_PREDATOR)) == (SHAPE_FADING | SHAPE_PREDATOR)) {
        flags = (flags & ~(SHAPE_FADING | SHAPE_PREDATOR)) | SHAPE_GHOST;
        ghost = special_ghost;
    }
    if (flags & ~(SHAPE_CENTER | SHAPE_WIN_REL | SHAPE_GHOST | SHAPE_FADING | SHAPE_TRANS)) {
        return false;
    }
    Shape* s = shape_of(file, frame, flags & SHAPE_FADING ? fade : NULL, flags & SHAPE_GHOST ? ghost : NULL);
    if (!s) {
        return false;
    }
    if (s->kind == NEW) {
        // (most frames a second worker core has ready: host.c)
        const struct host_frame* ready = (s->ghost && s->ghost == shroud_table) || check_plain ? NULL : host_frame(file, frame);
        if (check_frames < 0) {
            check_frames = getenv("TDAWN_CHECK_FRAMES") ? 1 + atoi(getenv("TDAWN_CHECK_FRAMES")) : 0;
            if (check_frames) {
                atexit(check_report);
            }
        }
        // (on the worker that draws the objects: Build_Frame's buffers and the heap are the first core's)
        const unsigned char* own = !ready && mcw_self() ? behind_frame(file, frame) : NULL;
        if (!ready && mcw_self() && !own) {
            drawing.missed = true; // (the object is drawn anew next frame)
            return true;
        }
        uintptr_t built = (ready && check_frames < 2) || own ? 1 : Build_Frame(file, (unsigned short)frame, _ShapeBuffer);
        s->w = s->full_w = Get_Build_Frame_Width(file);
        s->h = s->full_h = Get_Build_Frame_Height(file);
        s->left = s->top = 0;
        if (ready) {
            frames_taken++;
            if (check_frames > 1) {
                check_frame(s, ready, built);
            }
            if (ready->state == HOST_FRAME_EMPTY) {
                s->kind = EMPTY;
            } else {
                put(s, NULL, ready);
            }
        } else if (!built) {
            s->kind = EMPTY;
        } else if (own) {
            put(s, (unsigned char*)own);
        } else if (UseBigShapeBuffer) {
            ShapeHeader* header = (ShapeHeader*)built;
            put(s, (unsigned char*)(header->shape_buffer ? TheaterShapeBufferStart : BigShapeBufferStart) + (uintptr_t)header->shape_data);
        } else {
            put(s, (unsigned char*)built);
        }
    }
    if (s->kind == FAILED) {
        return false;
    }
    if (s->kind == EMPTY) {
        return true;
    }
    if (flags & SHAPE_CENTER) {
        x -= s->full_w / 2;
        y -= s->full_h / 2;
    }
    x += WindowList[window][WINDOWX] + s->left;
    y += WindowList[window][WINDOWY] + s->top;
    if (s->kind == SHADE) {
        emit(HOST_SHADE, x, y, s->w, s->h, s->x, s->y, 0x00ffffff);
        ShaderEmu_GL_Shroud++;
    } else {
        if (s->kind == MASKED) {
            emit(HOST_MASK, x, y, s->w, s->h, s->x, s->y, shade_of((const unsigned char*)s->ghost, 0));
            ShaderEmu_GL_Shadows++;
        }
        emit(HOST_KEYED, x, y, s->w, s->h, s->x, s->y, 0x00ffffff);
    }
    return true;
}

// An icon's 24 x 24 pixels into its square of the atlas, by the core that makes the scene
// (host_fill), with its own edge pixels once more around them: a cell's rectangle sampled on
// its edge, as it is over a window stretched by a fraction, then has the icon's own colours.
static void fill_icon(void* place, const void* from)
{
    unsigned char* to = (unsigned char*)place;

    for (int y = 0; y < 26; y++, to += ATLAS_W) {
        const unsigned char* row = (const unsigned char*)from + (y < 1 ? 0 : y > 24 ? 23 : y - 1) * 24;
        if (from) {
            to[0] = row[0];
            memcpy(to + 1, row, 24);
            to[25] = row[23];
        } else {
            memset(to, 0, 26);
        }
    }
}

// Where the icon's own pixels are in the atlas.
static inline int icon_x(int index)
{
    return index % ICON_ACROSS * 26 + 1;
}

static inline int icon_y(int index)
{
    return index / ICON_ACROSS * 26 + 1;
}

// Where an icon of a set is in the atlas, put there the first time. -1: no room.
static int icon_of(const void* set, int icon)
{
    unsigned at = ((uintptr_t)set * 31u + (unsigned)icon * 131u) % SLOTS;

    for (int tries = 0; tries < 64; tries++, at = (at + 1) % SLOTS) {
        Icon* i = &icons[at];
        if (i->set == set && i->icon == icon) {
            return i->index;
        }
        if (!i->set) {
            if (icon_count >= ICON_MOST) {
                return -1;
            }
            Init_Stamps((const IconControlType*)set);
            int which = MapPtr ? MapPtr[icon] : icon;
            unsigned char* to = host_atlas() + (icon_y(icon_count) - 1) * ATLAS_W + icon_x(icon_count) - 1;
            // (by the core that makes the scene: a first view of the map is some hundreds of them)
            host_fill(fill_icon, to, which < IconCount ? StampPtr + which * 576 : NULL);
            i->set = set;
            i->icon = (short)icon;
            i->index = (short)icon_count++;
            return i->index;
        }
    }
    return -1;
}

// A cell's own picture, as CellClass::Draw_It chooses it, the first time the cell is seen: its
// kept quad, which stays.
static void tile_make(CellClass* cellptr, int tile, int cx, int cy)
{
    TemplateTypeClass const& type =
        TemplateTypeClass::As_Reference(cellptr->TType != TEMPLATE_NONE ? cellptr->TType : TEMPLATE_CLEAR1);
    int index = type.Get_Image_Data()
                    ? icon_of(type.Get_Image_Data(), cellptr->TType != TEMPLATE_NONE ? cellptr->TIcon : cellptr->Clear_Icon())
                    : -1;

    if (index >= 0) {
        host_tile(tile, cx * 24, cy * 24, icon_x(index), icon_y(index));
    }
    tile_done[tile >> 3] |= 1 << (tile & 7);
}

// A terrain stamp of the map (the placing cursor's cells). False: the game is to draw it.
bool ShaderEmu_GL_Stamp(void const* set, int icon, int x, int y, void const* remap, int window)
{
    if (!ShaderEmu_GL_Emitting || window != WINDOW_TACTICAL || !set || remap) {
        return false;
    }
    int index = icon_of(set, icon);
    if (index < 0) {
        return false;
    }
    emit(HOST_KEYED, x + WindowList[window][WINDOWX], y + WindowList[window][WINDOWY], 24, 24, icon_x(index), icon_y(index), 0x00ffffff);
    return true;
}

// Whether the GPU draws the map: it does when it has room, and a quad for each of its cells.
/*
** The objects drawn by a worker core, a frame behind (docs/games/tdawn.md, "The objects a frame
** behind"): as Render comes, this core notes each object's part of the scene, copies those
** to be drawn anew as they are now, and does to the objects what Render would (their marks),
** and the worker draws the copies while the logic goes on. TDAWN_DRAW=here: as before.
*/
struct Behind
{
    ObjectClass* copy; // drawn anew from this; NULL: its kept quads as they are, or (away) none
    Kept* k;
    bool away;
};
enum { BEHIND_MOST = 1024, BEHIND_BYTES = 128 << 10 };
alignas(16) static Behind behind[BEHIND_MOST];
static int behind_count, behind_most = -1;
alignas(16) static unsigned char behind_arena[BEHIND_BYTES];
static bool behind_pending;
static int behind_not; // scenes whose objects are drawn here: the worker met a shape only the game draws
static int behind_rect[4];
static unsigned behind_drawn, behind_frames, behind_here;
bool ShaderEmu_GL_Behind; // (Main_Loop's Render: the objects may be drawn behind)

static int Behind_Size(ObjectClass* object)
{
    switch (object->What_Am_I()) {
    case RTTI_INFANTRY:
        return sizeof(InfantryClass);
    case RTTI_UNIT:
        return sizeof(UnitClass);
    case RTTI_BUILDING:
        return sizeof(BuildingClass);
    case RTTI_AIRCRAFT:
        return sizeof(AircraftClass);
    case RTTI_ANIM:
        return sizeof(AnimClass);
    case RTTI_BULLET:
        return sizeof(BulletClass);
    case RTTI_TERRAIN:
        return sizeof(TerrainClass);
    default:
        return 0;
    }
}

/*
** The objects' part of the scene noted, and what Render(true) does to an object done to it, in
** one pass; an object drawn anew in the view is copied (false: this frame's objects are drawn
** here, as before: what was done to the objects is what Render does again).
*/
static bool Behind_Note(int refresh, unsigned scenes, unsigned& away)
{
    if (behind_most < 0) {
        behind_most = !(getenv("TDAWN_DRAW") && !strcmp(getenv("TDAWN_DRAW"), "here"));
        memset(behind_buffer, 0, sizeof(behind_buffer)); // (its pages there before the worker stores to them)
        atexit([]() {
            if (getenv("TDAWN_FRAME_LOG") || getenv("TDAWN_MARK"))
                fprintf(stderr, "tdawn: %u scenes' objects drawn a frame behind (%u objects, %u frames made there), %u here\n",
                        behind_frames, behind_drawn, drawing.misses, behind_here);
        });
    }
    if (!behind_most || !ShaderEmu_GL_Behind || !host_scene_behind_ready()) {
        return false;
    }
    if (behind_not) {
        behind_not--;
        return false;
    }
    int n = 0;
    unsigned used = 0, passed = 0;
    COORDINATE view = Map.TacticalCoord;
    for (LayerType layer = LAYER_GROUND; layer < LAYER_COUNT; layer++) {
        int count = Map.Layer[layer].Count();
        int turn = refresh == 256 && count ? (int)(scenes % (unsigned)count) : -1;
        ObjectClass* const* list = count ? &Map.Layer[layer][0] : NULL; // (nothing here changes a layer)
        for (int index = 0; index < count; index++) {
            ObjectClass* object = list[index];
            bool anew = index == turn;
            if (object->ShaderEmu_Away == view && !object->IsToDisplay && !anew) {
                passed++;
                continue;
            }
            if (n == BEHIND_MOST) {
                return false;
            }
            Kept* k = kept_of(object);
            if (k && !anew && !object->IsToDisplay && (k->whole || (k->away && k->view == view))) {
                ShaderEmu_GL_Objects[k->whole ? 0 : 1]++;
                if (k->whole && (k->count || k->barred) && object->IsDown && !object->IsInLimbo) {
                    behind[n].copy = NULL, behind[n].away = false, behind[n++].k = k;
                }
                continue;
            }
            int size = Behind_Size(object);
            if (!size || used + size > BEHIND_BYTES) {
                return false;
            }
            // whether Render(true) draws it (in the view), then a copy as it is, then what Render does to it
            bool drew = false, shown = object->What_Am_I() != RTTI_ANIM || !((AnimClass*)object)->ShaderEmu_Delay();
            if (shown && object->IsDown && !object->IsInLimbo) {
                int x, y;
                drew = Map.Coord_To_Pixel(object->Render_Coord(), x, y);
            }
            behind[n].copy = NULL, behind[n].away = true, behind[n].k = k;
            if (drew) {
                behind[n].copy = (ObjectClass*)(behind_arena + used);
                memcpy((void*)behind[n].copy, (const void*)object, size);
                if (object->What_Am_I() == RTTI_ANIM) {
                    ((AnimClass*)behind[n].copy)->ShaderEmu_Detach(); // (what it is on may be gone before it is drawn)
                }
                used += (size + 15) & ~15;
                behind_drawn++;
            }
            if (shown) {
                object->IsToDisplay = !(object->IsDown && !object->IsInLimbo) && (object->What_Am_I() == RTTI_ANIM || object->IsToDisplay);
            }
            if (drew && object->Is_Techno()) {
                TechnoClass* techno = (TechnoClass*)object;
                techno->Clear_Redraw_Flag();
                if (techno->LineFrame < techno->LineMaxFrames) {
                    techno->LineFrame++; // (TechnoClass::Draw_It counts a line's frames: the logic reads that)
                }
            }
            object->ShaderEmu_Away = drew ? ~(COORDINATE)0 : view;
            n++;
        }
    }
    behind_count = n;
    away += passed;
    behind_frames++;
    return true;
}

/* On the worker: the noted objects into the scene's objects' part, the copies drawn. */
static void Behind_Draw(void)
{
    COORDINATE view = Map.TacticalCoord;

    ShaderEmu_GL_Emitting = true;
    for (int i = 0; i < behind_count; i++) {
        Kept* k = behind[i].k;
        if (!behind[i].copy) {
            if (!behind[i].away) {
                host_quads(k->quad, k->count);
                ShaderEmu_GL_Quads += k->count;
                if (k->barred) {
                    bars_put(k->bars);
                }
            } else if (k) {
                k->count = 0, k->whole = false, k->away = true, k->view = view; // (as Render left it, drawing nothing)
            }
            continue;
        }
        bool touched = ShaderEmu_GL_Touched;
        ShaderEmu_GL_Touched = false;
        ShaderEmu_GL_Objects[2]++;
        recording = k;
        if (k) {
            k->count = 0;
            k->whole = true;
            k->barred = false;
        }
        drawing.missed = false;
        bool drew = behind[i].copy->Render(true);
        recording = NULL;
        if (k) {
            k->whole = k->whole && drew && !ShaderEmu_GL_Touched && !drawing.missed;
            k->away = !drew;
            k->view = view;
        }
        ShaderEmu_GL_Touched |= touched;
    }
    ShaderEmu_GL_Emitting = false;
}

// The game's page between frames is the seen one: the copies draw into the hidden one, as Render
// does, and the game has its own back at the join (into the seen one their bars were lost).
static GraphicViewPortClass* behind_page;
static void Behind_Joined(void)
{
    Set_Logic_Page(behind_page);
    if (drawing.not_here) {
        drawing.not_here = false;
        behind_not = 256;
    }
}

// The game's own drawing of a shape, asked on the worker: not there (its buffers and heap are this
// core's), and the objects of the scenes after are drawn here.
bool ShaderEmu_GL_Not_Behind(void)
{
    if (!mcw_self()) {
        return false;
    }
    drawing.missed = drawing.not_here = true;
    return true;
}

/* After Main_Loop's Render (the sidebar drawn): the objects to a worker, if they were noted. */
void ShaderEmu_GL_Post(void)
{
    if (behind_pending) {
        behind_pending = false;
        behind_page = Set_Logic_Page(HidPage);
        host_scene_behind(Behind_Draw, behind_rect[0], behind_rect[1], behind_rect[2], behind_rect[3], KEY, Behind_Joined);
    }
}

bool ShaderEmu_GL_On(int cells)
{
    ShaderEmu_GL_Live = cells <= TILES && available();
    if (ShaderEmu_GL_Live) {
        void ShaderEmu_Foot_Overlaps_Off(void); // (map.cpp: what moves is out of the cells' overlap lists from now on)
        ShaderEmu_Foot_Overlaps_Off();
    }
    return ShaderEmu_GL_Live;
}

/*
** The map, in place of the game's own drawing of it. The page's part over the map is all
** holes, so what the game still draws there itself (text, bars, boxes) shows over the scene;
** it is made all holes again only after something was drawn there, and now and then.
*/
void DisplayClass::ShaderEmu_GL_Draw(bool forced)
{
    static struct
    {
        CELL cell; // the cell whose edge of the shroud this is, or -1: a run of cells not seen yet
        short x, y, w;
    } shroud[512];
    static int last_frame, since;
    int shrouded = 0, x, y;
    bool again; // nothing is taken from the frame before: now and then, in case a change went unmarked
    int wide = Lepton_To_Pixel(TacLeptonWidth), high = Lepton_To_Pixel(TacLeptonHeight);

    GraphicViewPortClass* page = LogicPage;
    host_scene_settle(); // (the scene before is made from the rectangles kept here)
    Set_Logic_Page(page); // (Render's: a join here gives back the page of between frames)
    if (strncmp(scenario, Scen.ScenarioName, sizeof(scenario) - 1) || (int)Frame < last_frame) {
        strncpy(scenario, Scen.ScenarioName, sizeof(scenario) - 1);
        forget();
        tile_x = MapCellX - 1;
        tile_y = MapCellY - 1;
        tile_w = MapCellWidth + 2;
        tile_h = MapCellHeight + 2;
        forced = true;
        static bool asked;
        if (!asked && getenv("TDAWN_ATLAS")) {
            forget_every = atoi(getenv("TDAWN_ATLAS"));
            atexit([]() { fprintf(stderr, "tdawn: the atlas's shapes forgotten %u times\n", shapes_forgotten); });
        }
        asked = true;
    }
    // (before a scene, with fewer rows left than one may want; TDAWN_ATLAS=N: every N scenes too, for a test)
    static unsigned scenes_now;
    if (drawing.full || shelf_top > ATLAS_ROWS - 256 || (forget_every && ++scenes_now % forget_every == 0)) {
        shapes_forget();
        forced = true;
    }
    last_frame = Frame;
    shroud_table = ShadowTrans;
    special_ghost = SpecialGhost;
    if (solid_x < 0 && room(24, 24, solid_x, solid_y)) {
        for (y = 0; y < 24; y++) {
            for (x = solid_x; x < solid_x + 24; x++) {
                host_masks()[(solid_y + y) * (ATLAS_W / 8) + (x >> 3)] |= 0x80 >> (x & 7);
            }
        }
    }

    // where the map's corner is, by the game's own arithmetic for the cell in the view's corner
    CELL corner = Coord_Cell(TacticalCoord);
    if (!Coord_To_Pixel(Coord_Whole(Cell_Coord(corner)), x, y)) {
        x = Cell_X(corner) * 24 - Lepton_To_Pixel(Coord_X(TacticalCoord));
        y = Cell_Y(corner) * 24 - Lepton_To_Pixel(Coord_Y(TacticalCoord));
    }
    int base_x = x - Cell_X(corner) * 24, base_y = y - Cell_Y(corner) * 24;
    shown_x0 = Cell_X(corner) - 1;
    shown_y0 = Cell_Y(corner) - 1;
    shown_x1 = Cell_X(corner) + wide / 24 + 2;
    shown_y1 = Cell_Y(corner) + high / 24 + 2;
    host_scene_begin(TacPixelX + base_x, TacPixelY + base_y);
    host_scene_view(TacPixelX, TacPixelY, TacPixelX + wide, TacPixelY + high);
    origin_x = TacPixelX + base_x;
    origin_y = TacPixelY + base_y;
    view_w = TacPixelX + wide;
    view_h = TacPixelY + high;
    // TDAWN_REFRESH=N: every N scenes (256; 0 for never). At 256 the objects have their turns, one
    // of a layer a scene, and the page is not filled for it: all in one scene was a long frame.
    static int refresh = -1;
    static unsigned scenes;
    if (refresh < 0) {
        refresh = getenv("TDAWN_REFRESH") ? atoi(getenv("TDAWN_REFRESH")) : 256;
    }
    bool all = forced || kept_count > KEPT * 3 / 4; // every object anew, and the page's holes
    again = all || (refresh && ++since >= refresh);
    all |= again && refresh != 256;
    scenes++;
    if (again) {
        since = 0;
        if (kept_count > KEPT / 2) {
            memset(kept, 0, sizeof(kept)); // (objects come and go: their places are found again)
            kept_count = 0;
        }
    }
    if (all || ShaderEmu_GL_Touched) {
        fills++;
        LogicPage->Fill_Rect(TacPixelX, TacPixelY, TacPixelX + wide - 1, TacPixelY + high - 1, KEY);
    } else if (box[0] <= box[2]) {
        // (only what of it is over the map, and nothing if none of it is: the game's fill takes a
        // right edge left of the left one as a width below nothing, and its memset then went from
        // the page through all of the GPU's memory. Something drawn over the sidebar did that.)
        int fx0 = MAX(box[0], TacPixelX), fy0 = MAX(box[1], TacPixelY);
        int fx1 = MIN(box[2], TacPixelX + wide - 1), fy1 = MIN(box[3], TacPixelY + high - 1);
        if (fx0 <= fx1 && fy0 <= fy1) {
            fills++;
            LogicPage->Fill_Rect(fx0, fy0, fx1, fy1, KEY);
        }
    }
    if (bars.count && !all && !ShaderEmu_GL_Touched && LogicPage->Lock()) {
        Pen p = {(unsigned char*)LogicPage->Get_Offset(), LogicPage->Get_Width() + LogicPage->Get_XAdd() + LogicPage->Get_Pitch(),
                 TacPixelX, TacPixelY, TacPixelX + wide - 1, TacPixelY + high - 1};
        for (int i = 0; i < bars.count; i++) {
            const BarsDraw& b = bars.drawn[i];
            fills += MIN(b.y - b.height / 2, b.y - b.ly) <= TacPixelY + message_rows; // (the messages are printed again over it)
            bars_pen(p, b, b.x, b.y, KEY);
        }
        LogicPage->Unlock();
    }
    bars.count = 0;
    ShaderEmu_GL_Touched = false;
    stale = false;
    box[0] = 1, box[2] = 0;

    ShaderEmu_GL_Emitting = true;
    unsigned mark = host_cycles();
    bool under = again || under_stale || under_view[0] != origin_x || under_view[1] != origin_y || under_view[2] != wide
                 || under_view[3] != high;
    under_stale = false;
    under_view[0] = origin_x, under_view[1] = origin_y, under_view[2] = wide, under_view[3] = high;
    if (under) {
        host_scene_part(0, 0);
    }
    for (int cy = Cell_Y(corner); under && cy < MAP_CELL_H && (y = base_y + cy * 24) < high; cy++) {
        int run = -1; // where a run of cells not seen yet began
        for (int cx = Cell_X(corner); cx < MAP_CELL_W && (x = base_x + cx * 24) < wide; cx++) {
            CELL cell = XY_Cell(cx, cy);
            CellClass* cellptr = &(*this)[cell];
            bool seen = cellptr->Is_Visible(PlayerPtr) || Debug_Unshroud;
            int tx = cx - tile_x, ty = cy - tile_y;
            int tile = tx >= 0 && ty >= 0 && tx < tile_w && ty < tile_h ? ty * tile_w + tx : -1;
            if (seen) {
                if (tile >= 0 && !(tile_done[tile >> 3] & (1 << (tile & 7)))) {
                    tile_make(cellptr, tile, cx, cy);
                }
                if (cellptr->Smudge != SMUDGE_NONE || cellptr->Overlay != OVERLAY_NONE || cellptr->IsCursorHere) {
                    cellptr->Draw_It(x, y, CELL_DRAW_ONLY);
                    under_stale |= cellptr->IsCursorHere; // (the placing cursor's cells are asked every time)
                }
            }
            if (seen || run < 0 || shrouded >= 512) {
                run = -1;
            } else {
                shroud[run].w += 24; // one rectangle for the cells not seen in a row
            }
            if (!seen && run < 0 && shrouded < 512) {
                run = shrouded;
                shroud[shrouded].cell = -1;
                shroud[shrouded].x = (short)x;
                shroud[shrouded].y = (short)y;
                shroud[shrouded++].w = 24;
            } else if (seen && !cellptr->Is_Mapped(PlayerPtr) && !Debug_Unshroud && shrouded < 512) {
                // which edge of the shroud: worked out again when the game flags the cell
                int edge = tile < 0 || again || edge_of[tile] == -2 || Is_Cell_Flagged(cell)
                               ? Cell_Shadow(cell, PlayerPtr)
                               : edge_of[tile];
                if (tile >= 0) {
                    edge_of[tile] = (signed char)edge;
                }
                if (edge >= 0) {
                    shroud[shrouded].cell = cell;
                    shroud[shrouded].x = (short)x;
                    shroud[shrouded].y = (short)y;
                    shroud[shrouded++].w = (short)edge;
                }
            }
        }
    }
    under_stale |= ShaderEmu_GL_Touched; // (a cell the game drew into the page itself is drawn every time)
    ShaderEmu_GL_Parts[0] += host_cycles() - mark;
    mark = host_cycles();
    host_scene_part(1, 0);
    unsigned away = 0;
    bool drawn_behind = !all && Behind_Note(refresh, scenes, away);
    behind_here += !drawn_behind;
    for (LayerType layer = LAYER_GROUND; layer < LAYER_COUNT && !drawn_behind; layer++) {
        int turn = refresh == 256 && Layer[layer].Count() ? (int)(scenes % (unsigned)Layer[layer].Count()) : -1;
        for (int index = 0; index < Layer[layer].Count(); index++) {
            ObjectClass* object = Layer[layer][index];
            // (most objects are out of the view and were not marked: passed over at once, by what
            // the object itself has of the view at which it drew nothing)
            bool anew = all || index == turn;
            if (object->ShaderEmu_Away == TacticalCoord && !object->IsToDisplay && !anew) {
                away++;
                continue;
            }
            Kept* k = kept_of(object);
            if (k && !anew && !object->IsToDisplay && (k->whole || (k->away && k->view == TacticalCoord))) {
                // as it was: its rectangles where they are on the map now, or still nothing
                ShaderEmu_GL_Objects[k->whole ? 0 : 1]++;
                if (k->whole && (k->count || k->barred) && object->IsDown && !object->IsInLimbo) {
                    host_quads(k->quad, k->count); // (those of them in the view, where they are on the map now)
                    ShaderEmu_GL_Quads += k->count;
                    if (k->barred) {
                        bars_put(k->bars);
                    }
                }
                continue;
            }
            bool touched = ShaderEmu_GL_Touched;
            ShaderEmu_GL_Touched = false;
            ShaderEmu_GL_Objects[2]++;
            recording = k;
            if (k) {
                k->count = 0;
                k->whole = true;
                k->barred = false;
            }
            bool drew = object->Render(true);
            recording = NULL;
            if (k) {
                k->whole = k->whole && drew && !ShaderEmu_GL_Touched;
                k->away = !drew;
                k->view = TacticalCoord;
            }
            object->ShaderEmu_Away = drew ? ~(COORDINATE)0 : TacticalCoord;
            ShaderEmu_GL_Touched |= touched;
        }
    }
    ShaderEmu_GL_Objects[1] += away;
    ShaderEmu_GL_Parts[1] += host_cycles() - mark;
    mark = host_cycles();
    // the shroud: first what was never seen, then its edges (each kind is one command that way)
    host_scene_part(2, !under);
    for (int i = 0; i < shrouded && solid_x >= 0; i++) {
        if (shroud[i].cell == -1) {
            host_block(TacPixelX + shroud[i].x, TacPixelY + shroud[i].y, shroud[i].w, 24, solid_x, solid_y, 0);
            ShaderEmu_GL_Quads++;
            ShaderEmu_GL_Shroud++;
        }
    }
    for (int i = 0; i < shrouded; i++) {
        int edge = shroud[i].w;
        Shape* s = shroud[i].cell != -1 && edge < 64 ? edge_shape[edge] : NULL;
        if (s && s->kind == SHADE) {
            emit(HOST_SHADE, TacPixelX + shroud[i].x, TacPixelY + shroud[i].y, s->w, s->h, s->x, s->y, 0x00ffffff);
            ShaderEmu_GL_Shroud++;
        } else if (shroud[i].cell != -1) {
            // the first time by the game's own call, which puts the shape in the shades
            CC_Draw_Shape(ShadowShapes, edge, shroud[i].x, shroud[i].y, WINDOW_TACTICAL, SHAPE_GHOST, NULL, ShadowTrans);
            if (edge < 64) {
                edge_shape[edge] = shape_of(ShadowShapes, edge, NULL, ShadowTrans);
            }
        }
    }
    ShaderEmu_GL_Parts[2] += host_cycles() - mark;
    ShaderEmu_GL_Emitting = false;
    // (what the game draws over the map after this: a band being dragged, its messages)
    if (IsRubberBand) {
        ShaderEmu_GL_Mark(TacPixelX + MIN(BandX, NewX) - 1, TacPixelY + MIN(BandY, NewY) - 1,
                          TacPixelX + MAX(BandX, NewX) + 1, TacPixelY + MAX(BandY, NewY) + 1);
    }
    if (drawn_behind) {
        host_scene_part(1, 1); // (the worker's notes are the objects')
        behind_pending = true;
        behind_rect[0] = TacPixelX, behind_rect[1] = TacPixelY, behind_rect[2] = wide, behind_rect[3] = high;
        return;
    }
    host_scene_end(TacPixelX, TacPixelY, wide, high, KEY);
}
