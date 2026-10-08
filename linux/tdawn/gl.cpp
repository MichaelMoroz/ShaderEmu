// Tiberian Dawn's map drawn by the machine's GPU (docs/tdawn.md). The terrain is quads that
// stay, one a cell; overlays, objects and the shroud are quads of each frame, from atlases
// their pictures are put into the first time they are drawn. The game's own page goes on top.
#include "function.h"
#include "common/host.h"
#include "common/keyframe.h"

enum
{
    ATLAS_W = 1024,
    ATLAS_ROWS = 2048,
    ICON_ROWS = 1008, // the atlas's first rows hold terrain icons, 42 x 42 of them
    ICON_MOST = 42 * 42,
    SHADE_MOST = 5 * 10, // the shroud's shapes, in the shades
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

bool ShaderEmu_GL_Emitting;       // the map's shapes and stamps go to the GPU
bool ShaderEmu_GL_Live;           // the GPU draws the map: a changed cell does not have its objects drawn again
bool ShaderEmu_GL_Touched = true; // the game drew over the map itself: the page has more than holes there
static int box[4] = {1, 1, 0, 0}; // or only inside this rectangle of the page (x0, y0, x1, y1)

// An object's rectangles as it drew them last, by their place on the map: drawn again as they
// are while the game has not marked the object as changed.
struct Quad
{
    short x, y, w, h, ax, ay;
    unsigned char kind;
    unsigned colour;
};
struct Kept
{
    const ObjectClass* who;
    unsigned char count;
    bool whole; // these are all of it: nothing the game drew itself, and no more than fit
    bool away;  // or it drew nothing, being out of the view when the view was at...
    COORDINATE view;
    Quad quad[6];
};
enum { KEPT = 1024 };
static Kept kept[KEPT];
static int kept_count;
static Kept* recording;
static int origin_x, origin_y, view_w, view_h; // where the map's corner is on the screen, and the view's size
unsigned ShaderEmu_GL_Quads;      // all of them, and of those the shroud's and the shadows'
unsigned ShaderEmu_GL_Shroud, ShaderEmu_GL_Shadows;
unsigned ShaderEmu_GL_Parts[3]; // instructions in the cells, the objects and the shroud
unsigned ShaderEmu_GL_Objects[3]; // objects shown as they were, found out of the view again, and drawn anew

static Shape shapes[SLOTS];
static Icon icons[SLOTS];
static int icon_count, shade_count;
static int shelf_x[33], shelf_y[33], shelf_top; // a row of the atlas for each height, in 16s
static unsigned char tile_done[TILES / 8];
static signed char edge_of[TILES]; // the shroud's shape over a cell, as last worked out (-2: not yet)
static Shape* edge_shape[64];      // and where each of those shapes is in the shades
static int tile_x, tile_y, tile_w, tile_h; // the map's cells that have a quad
static int shown_x0, shown_y0, shown_x1, shown_y1; // the cells of the view drawn last
static bool stale; // something in that view changed since
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

static void forget(void)
{
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

// Something of this cell changed (CellClass::Redraw_Objects): its edge of the shroud is worked
// out again, and the scene made again if the cell is in it.
void ShaderEmu_GL_Cell(CELL cell)
{
    int x = Cell_X(cell), y = Cell_Y(cell), tx = x - tile_x, ty = y - tile_y;

    if (tx >= 0 && ty >= 0 && tx < tile_w && ty < tile_h) {
        edge_of[ty * tile_w + tx] = -2;
    }
    if (!stale && x >= shown_x0 && x <= shown_x1 && y >= shown_y0 && y <= shown_y1) {
        stale = true;
        Map.Flag_To_Redraw(false);
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
        for (int index = 0; index < Map.Layer[layer].Count(); index++) {
            const ObjectClass* object = Map.Layer[layer][index];
            all += (uintptr_t)object ^ (uintptr_t)object >> 7; // (whatever their order)
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

static Kept* kept_of(const ObjectClass* who)
{
    unsigned at = (unsigned)((uintptr_t)who >> 3) % KEPT;

    for (int tries = 0; tries < 16; tries++, at = (at + 1) % KEPT) {
        if (kept[at].who == who) {
            return &kept[at];
        }
        if (!kept[at].who) {
            kept[at].who = who;
            kept[at].whole = kept[at].away = false;
            kept_count++;
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

static void put(Shape* s, const unsigned char* pixels)
{
    const unsigned char* fade = (const unsigned char*)s->fade;
    const unsigned char* ghost = (const unsigned char*)s->ghost;
    int x, y;

    if (ghost && ghost == shroud_table) {
        // the shroud: every level of dark in one picture of words
        if (s->w > 24 || s->h > 24 || shade_count >= SHADE_MOST) {
            s->kind = FAILED;
            return;
        }
        unsigned level[256];
        bool known[256] = {false};
        s->x = shade_count % 5 * 24;
        s->y = shade_count / 5 * 24;
        shade_count++;
        for (y = 0; y < s->h; y++) {
            unsigned* to = host_shades() + (s->y + y) * 128 + s->x;
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
        s->kind = SHADE;
        return;
    }
    // only the rectangle that has pixels is kept: a soldier is a small figure in a large frame
    int ax, ay, x0 = s->w, x1 = -1, y0 = -1, y1 = -1, full_w = s->w;
    for (y = 0; y < s->h; y++) {
        const unsigned char* row = pixels + y * full_w;
        int a = 0, b = full_w - 1;
        while (a < full_w && !row[a]) {
            a++;
        }
        if (a == full_w) {
            continue;
        }
        while (!row[b]) {
            b--;
        }
        y0 = y0 < 0 ? y : y0;
        y1 = y;
        x0 = MIN(x0, a);
        x1 = MAX(x1, b);
    }
    if (y0 < 0) {
        s->kind = EMPTY;
        return;
    }
    if (!room(x1 - x0 + 1, y1 - y0 + 1, ax, ay)) {
        s->kind = FAILED;
        return;
    }
    s->left = (short)x0;
    s->top = (short)y0;
    s->w = (short)(x1 - x0 + 1);
    s->h = (short)(y1 - y0 + 1);
    pixels += y0 * full_w + x0;
    s->x = ax;
    s->y = ay;
    s->kind = KEYED;
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

// A shape of the map, as CC_Draw_Shape was asked for it. False: the game is to draw it.
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
        uintptr_t built = Build_Frame(file, (unsigned short)frame, _ShapeBuffer);
        s->w = s->full_w = Get_Build_Frame_Width(file);
        s->h = s->full_h = Get_Build_Frame_Height(file);
        s->left = s->top = 0;
        if (!built) {
            s->kind = EMPTY;
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
            unsigned char* to = host_atlas() + (icon_count / 42 * 24) * ATLAS_W + icon_count % 42 * 24;
            for (int y = 0; y < 24; y++, to += ATLAS_W) {
                if (which < IconCount) {
                    memcpy(to, StampPtr + which * 576 + y * 24, 24);
                } else {
                    memset(to, 0, 24);
                }
            }
            i->set = set;
            i->icon = (short)icon;
            i->index = (short)icon_count++;
            return i->index;
        }
    }
    return -1;
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
    emit(HOST_KEYED, x + WindowList[window][WINDOWX], y + WindowList[window][WINDOWY], 24, 24, index % 42 * 24, index / 42 * 24, 0x00ffffff);
    return true;
}

// Whether the GPU draws the map: it does when it has room, and a quad for each of its cells.
bool ShaderEmu_GL_On(int cells)
{
    ShaderEmu_GL_Live = cells <= TILES && available();
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

    if (strncmp(scenario, Scen.ScenarioName, sizeof(scenario) - 1) || (int)Frame < last_frame) {
        strncpy(scenario, Scen.ScenarioName, sizeof(scenario) - 1);
        forget();
        tile_x = MapCellX - 1;
        tile_y = MapCellY - 1;
        tile_w = MapCellWidth + 2;
        tile_h = MapCellHeight + 2;
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
    origin_x = TacPixelX + base_x;
    origin_y = TacPixelY + base_y;
    view_w = TacPixelX + wide;
    view_h = TacPixelY + high;
    static int refresh = -1; // TDAWN_REFRESH=N: every N frames (32); 0 for never
    if (refresh < 0) {
        refresh = getenv("TDAWN_REFRESH") ? atoi(getenv("TDAWN_REFRESH")) : 32;
    }
    again = forced || (refresh && ++since >= refresh) || kept_count > KEPT * 3 / 4;
    if (again) {
        since = 0;
        if (kept_count > KEPT / 2) {
            memset(kept, 0, sizeof(kept)); // (objects come and go: their places are found again)
            kept_count = 0;
        }
    }
    if (again || ShaderEmu_GL_Touched) {
        LogicPage->Fill_Rect(TacPixelX, TacPixelY, TacPixelX + wide - 1, TacPixelY + high - 1, KEY);
    } else if (box[0] <= box[2]) {
        LogicPage->Fill_Rect(MAX(box[0], TacPixelX), MAX(box[1], TacPixelY), MIN(box[2], TacPixelX + wide - 1),
                             MIN(box[3], TacPixelY + high - 1), KEY);
    }
    ShaderEmu_GL_Touched = false;
    stale = false;
    box[0] = 1, box[2] = 0;

    ShaderEmu_GL_Emitting = true;
    unsigned mark = host_cycles();
    for (int cy = Cell_Y(corner); cy < MAP_CELL_H && (y = base_y + cy * 24) < high; cy++) {
        int run = -1; // where a run of cells not seen yet began
        for (int cx = Cell_X(corner); cx < MAP_CELL_W && (x = base_x + cx * 24) < wide; cx++) {
            CELL cell = XY_Cell(cx, cy);
            CellClass* cellptr = &(*this)[cell];
            bool seen = cellptr->Is_Visible(PlayerPtr) || Debug_Unshroud;
            int tx = cx - tile_x, ty = cy - tile_y;
            int tile = tx >= 0 && ty >= 0 && tx < tile_w && ty < tile_h ? ty * tile_w + tx : -1;
            if (seen) {
                if (tile >= 0 && !(tile_done[tile >> 3] & (1 << (tile & 7)))) {
                    // the cell's own picture, as CellClass::Draw_It chooses it: from now on it stays
                    TemplateTypeClass const& type = TemplateTypeClass::As_Reference(
                        cellptr->TType != TEMPLATE_NONE ? cellptr->TType : TEMPLATE_CLEAR1);
                    int index = type.Get_Image_Data()
                                    ? icon_of(type.Get_Image_Data(),
                                              cellptr->TType != TEMPLATE_NONE ? cellptr->TIcon : cellptr->Clear_Icon())
                                    : -1;
                    if (index >= 0) {
                        host_tile(tile, cx * 24, cy * 24, index % 42 * 24, index / 42 * 24);
                    }
                    tile_done[tile >> 3] |= 1 << (tile & 7);
                }
                if (cellptr->Smudge != SMUDGE_NONE || cellptr->Overlay != OVERLAY_NONE || cellptr->IsCursorHere) {
                    cellptr->Draw_It(x, y, CELL_DRAW_ONLY);
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
    ShaderEmu_GL_Parts[0] += host_cycles() - mark;
    mark = host_cycles();
    for (LayerType layer = LAYER_GROUND; layer < LAYER_COUNT; layer++) {
        for (int index = 0; index < Layer[layer].Count(); index++) {
            ObjectClass* object = Layer[layer][index];
            Kept* k = kept_of(object);
            if (k && !again && !object->IsToDisplay && (k->whole || (k->away && k->view == TacticalCoord))) {
                // as it was: its rectangles where they are on the map now, or still nothing
                ShaderEmu_GL_Objects[k->whole ? 0 : 1]++;
                for (int i = 0; k->whole && i < k->count && object->IsDown && !object->IsInLimbo; i++) {
                    const Quad& q = k->quad[i];
                    int qx = q.x + origin_x, qy = q.y + origin_y;
                    if (qx < view_w && qy < view_h && qx + q.w > TacPixelX && qy + q.h > TacPixelY) {
                        host_sprite(q.kind, qx, qy, q.w, q.h, q.ax, q.ay, q.colour);
                        ShaderEmu_GL_Quads++;
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
            }
            bool drew = object->Render(true);
            recording = NULL;
            if (k) {
                k->whole = k->whole && drew && !ShaderEmu_GL_Touched;
                k->away = !drew;
                k->view = TacticalCoord;
            }
            ShaderEmu_GL_Touched |= touched;
        }
    }
    ShaderEmu_GL_Parts[1] += host_cycles() - mark;
    mark = host_cycles();
    // the shroud: first what was never seen, then its edges (each kind is one command that way)
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
    if (Messages.Num_Messages() > 0) {
        ShaderEmu_GL_Mark(TacPixelX, TacPixelY, TacPixelX + wide - 1, TacPixelY + 10 * Messages.Num_Messages() + 10);
    }
    host_scene_end(TacPixelX, TacPixelY, wide, high, KEY);
}
