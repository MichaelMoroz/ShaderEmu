// SPDX-License-Identifier: GPL-3.0-or-later
// ShaderEmu: OpenCNC3D's mission drawn on the machine's GPU from its pack, with no menus and no sidebar
// (docs/games/opencnc3d.md, "The picture"). Not OpenCNC3D's eyes (game/cnc_eyes.cpp, 47,600 lines with
// SDL, its modules and their packs: "What is left"), but its pack and its conventions, read out of that file:
// world x east, y up, z south in cells; a model unit 1/1024 of a cell (MODEL_SCALE); a corner of the PK9
// heightmap 1/64 of a cell; a facing 0..255 clockwise from north turning a model by -face * 2 pi / 256
// (facing_rot); the cartridge's camera, 50 degrees of field (N64_FOVY_DEG). The GL is Half-Life's shim
// (linux/halflife/gl_shaderemu.c, included as it is, as linux/jk2/gl_jk2.c does) over Quake III's draws.
//   opencnc3d CONTENT/ MISSIONS/ SCEN PACK [FRAMES]
//   OPENCNC3D_LOOK=X,Y   the cell the camera looks at (the player's MCV, or the map's middle)
//   OPENCNC3D_DIST=D     the camera's distance in cells (16); OPENCNC3D_PITCH=DEG (55)
//   OPENCNC3D_TICKS=N    game ticks before the first picture (0); one a frame after it
#include "../halflife/gl_shaderemu.c"
#include <math.h>
#include <nano-X.h>

// what gl_shaderemu.c asks of Half-Life's drawing on a worker, which this program does not have
int SE_RenderWho(void) { return 0; }
int SE_RenderOut(void) { return 0; }
uint32_t SE_RenderOnCore0(uint32_t (*fn)(void*), void* args) { return fn(args); }

// the brain (brain_shaderemu.cpp)
int ShaderEmu_Start(const char* content, const char* dir, const char* scen, int build_level, int seed);
int ShaderEmu_Advance(void);
unsigned ShaderEmu_State_Sum(int* counts);
void ShaderEmu_Set_Legacy_Render(int on);
typedef struct { int kind; char type[12]; int house, x, y, face, turret, strength, max_strength; } SE_Object;
int ShaderEmu_Objects(SE_Object* out, int max);

static unsigned instructions(void) {
    unsigned n;
    __asm__ volatile("rdinstret %0" : "=r"(n));
    return n;
}

// ---- the pack (CNC3DPKG v16, OpenCNC3D's tools/bakery/packinspect.py says every field) ----
typedef struct { int ti; unsigned char mode, wrap; float v[3][5]; unsigned char c[3][4]; } Tri;
typedef struct { char name[17]; int ntri; Tri* tri; int nparts; int* part_tri0; int* part_n; int* part_role; float* pivot; } Mesh;
typedef struct { int w, h, uw, uh; unsigned gl; int cutout; } Tex;
typedef struct { char code[9]; int mesh; } Type;
typedef struct { float u0, v0, u1, v1; int holes; } Cell;
static Tex* tex; static int ntex;
static Mesh* mesh; static int nmesh;
static Type* type; static int ntype;
static int terrain_tex, water1, water2, seabed, mapw = 64, maph = 64;
static Cell* cell;
// the infantry strips: frames stacked down the texture, the facings' frames one after another
typedef struct { int ti, frames, facings, fw, fh, w, h; } Sprite;
typedef struct { char code[9]; int strip[6]; } Inf;   // strip[house * 3 + anim]: GDI, Nod; STAND, RUN, DETH1
static Sprite* sprite; static int nsprite;
static Inf* inf; static int ninf;
static unsigned char* corner;     // (mapw+1)*(maph+1) heights, 1/64 cell
static unsigned pack_ram;         // what of the pack stays in RAM: meshes, cells, heights

static const unsigned char* pk;
static unsigned u32(void) { unsigned v; memcpy(&v, pk, 4); pk += 4; return v; }
static float f32(void) { float v; memcpy(&v, pk, 4); pk += 4; return v; }
static void* keep(unsigned n) { pack_ram += n; return calloc(1, n ? n : 1); }

static int load_pack(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* b = malloc((size_t)size);
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) { fclose(f); return -1; }
    fclose(f);
    if (memcmp(b, "CNC3DPKG", 8)) { fprintf(stderr, "opencnc3d: %s is not a CNC3DPKG pack\n", path); return -1; }
    pk = b + 8;
    u32();                       // version 16
    mapw = (int)u32(), maph = (int)u32();
    pk += 32;                    // scenario, theater
    ntex = (int)u32();
    tex = keep((unsigned)ntex * sizeof(Tex));
    s_glEnable(GL_TEXTURE_2D);
    for (int i = 0; i < ntex; i++) {
        Tex* t = &tex[i];
        t->w = (int)u32(), t->h = (int)u32(), t->uw = (int)u32(), t->uh = (int)u32();
        const unsigned char* px = pk;
        pk += (size_t)t->w * t->h * 4;
        for (int k = 3; k < t->w * t->h * 4; k += 4) if (px[k] == 0) { t->cutout = 1; break; }
        s_glGenTextures(1, &t->gl);
        s_glBindTexture(GL_TEXTURE_2D, t->gl);
        s_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (*pk++) pk += (size_t)t->w * t->h * 4;   // the GDI house variant: not drawn yet
    }
    nmesh = (int)u32();
    mesh = keep((unsigned)nmesh * sizeof(Mesh));
    for (int i = 0; i < nmesh; i++) {
        Mesh* m = &mesh[i];
        memcpy(m->name, pk, 16), pk += 16;
        m->ntri = (int)u32();
        m->tri = keep((unsigned)m->ntri * sizeof(Tri));
        for (int t = 0; t < m->ntri; t++) {
            Tri* r = &m->tri[t];
            memcpy(&r->ti, pk, 4), pk += 4;
            r->mode = *pk++, r->wrap = *pk++;
            for (int k = 0; k < 3; k++) {
                for (int q = 0; q < 5; q++) r->v[k][q] = f32();
                memcpy(r->c[k], pk, 4), pk += 4;
            }
        }
        m->nparts = (int)u32();
        m->part_tri0 = keep((unsigned)m->nparts * 4), m->part_n = keep((unsigned)m->nparts * 4);
        m->part_role = keep((unsigned)m->nparts * 4), m->pivot = keep((unsigned)m->nparts * 12);
        for (int p = 0; p < m->nparts; p++) {
            m->part_tri0[p] = (int)u32(), m->part_n[p] = (int)u32(), m->part_role[p] = (int)u32();
            for (int q = 0; q < 3; q++) m->pivot[p * 3 + q] = f32();
        }
        unsigned ns = u32();
        pk += ns * 4;            // construction sections
        unsigned nf = u32(); u32(); unsigned nclip = u32();
        pk += nclip * 9;
        if (nf) pk += (size_t)nf * m->nparts * 48 + (size_t)nf * m->nparts;   // node animation: not played yet
    }
    ntype = (int)u32();
    type = keep((unsigned)ntype * sizeof(Type));
    for (int i = 0; i < ntype; i++) {
        memcpy(type[i].code, pk, 8), pk += 8;
        memcpy(&type[i].mesh, pk, 4), pk += 4;
        pk++;                    // confidence
    }
    terrain_tex = (int)u32();
    u32();                       // the DOS atlas: -1 in our packs
    unsigned ncell = u32();
    cell = keep((unsigned)(mapw * maph) * sizeof(Cell));
    for (unsigned i = 0; i < ncell; i++) {
        short x, y;
        memcpy(&x, pk, 2), memcpy(&y, pk + 2, 2), pk += 4;
        Cell c;
        c.u0 = f32(), c.v0 = f32(), c.u1 = f32(), c.v1 = f32();
        c.holes = *pk++;
        if (x >= 0 && x < mapw && y >= 0 && y < maph) cell[y * mapw + x] = c;
    }
    nsprite = (int)u32();
    sprite = keep((unsigned)nsprite * sizeof(Sprite));
    for (int i = 0; i < nsprite; i++) {
        pk += 16;
        Sprite* s = &sprite[i];
        s->ti = (int)u32(), s->frames = (int)u32(), s->facings = (int)u32();
        s->fw = (int)u32(), s->fh = (int)u32(), s->w = (int)u32(), s->h = (int)u32();
    }
    ninf = (int)u32();
    inf = keep((unsigned)ninf * sizeof(Inf));
    for (int i = 0; i < ninf; i++) {
        memcpy(inf[i].code, pk, 8), pk += 12;   // the code, then its set's name
        for (int k = 0; k < 6; k++) memcpy(&inf[i].strip[k], pk, 4), pk += 4;
    }
    // the tail, from the end: PKC tint, PK9 heights, PKA seabed, PK8 water
    unsigned nc = (unsigned)((mapw + 1) * (maph + 1));
    corner = keep(nc);
    memcpy(corner, b + size - 12 - nc, nc);
    memcpy(&seabed, b + size - 12, 4);
    memcpy(&water1, b + size - 8, 4);
    memcpy(&water2, b + size - 4, 4);
    free(b);
    return 0;
}

// a type's mesh, remembered by its name (a walk of the 592 types each object was 9% of a frame)
static struct { char code[9]; int mesh; } known[256];
static int mesh_of(const char* code) {
    unsigned h = 2166136261u;
    for (const char* c = code; *c && c < code + 8; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    h &= 255;
    if (known[h].code[0] && !strncmp(known[h].code, code, 8)) return known[h].mesh;
    int m = -1;
    for (int i = 0; i < ntype; i++)
        if (!strncmp(type[i].code, code, 8)) { m = type[i].mesh; break; }
    strncpy(known[h].code, code, 8), known[h].mesh = m;
    return m;
}
static float corner_y(int x, int z) {
    if (x < 0) x = 0; else if (x > mapw) x = mapw;
    if (z < 0) z = 0; else if (z > maph) z = maph;
    return corner[z * (mapw + 1) + x] * (1.0f / 64);
}
static float ground_y(float x, float z) {
    int ix = (int)floorf(x), iz = (int)floorf(z);
    float fx = x - ix, fz = z - iz;
    float a = corner_y(ix, iz) * (1 - fx) + corner_y(ix + 1, iz) * fx;
    float b2 = corner_y(ix, iz + 1) * (1 - fx) + corner_y(ix + 1, iz + 1) * fx;
    return a * (1 - fz) + b2 * fz;
}

// ---- drawing ----
static int win_w = 640, win_h = 480;
static float look_x = 32, look_z = 32, dist = 16, pitch = 55;

static void bind(int ti, int wrap) {
    if (ti < 0) { s_glDisable(GL_TEXTURE_2D); return; }
    s_glEnable(GL_TEXTURE_2D);
    s_glBindTexture(GL_TEXTURE_2D, tex[ti].gl);
    s_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (wrap & 2) ? GL_CLAMP_TO_EDGE : 0x2901);
    s_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (wrap & 8) ? GL_CLAMP_TO_EDGE : 0x2901);
}

static void draw_terrain(void) {
    int x0 = (int)look_x - 14, x1 = (int)look_x + 14, z0 = (int)look_z - 18, z1 = (int)look_z + 8;
    if (x0 < 0) x0 = 0; if (z0 < 0) z0 = 0; if (x1 > mapw) x1 = mapw; if (z1 > maph) z1 = maph;
    // the sea floor and the sea under the cells whose art has holes (PKA, PK8): the floor at the cell's
    // corners, the water a twentieth of a cell above them, blended at the cartridge's 170/255
    s_glColor4ub(255, 255, 255, 255);
    bind(seabed, 0);
    s_glBegin(GL_TRIANGLES);
    for (int z = z0; z < z1; z++)
        for (int x = x0; x < x1; x++) {
            if (!cell[z * mapw + x].holes) continue;
            float y00 = corner_y(x, z) - 0.15f, y10 = corner_y(x + 1, z) - 0.15f, y01 = corner_y(x, z + 1) - 0.15f, y11 = corner_y(x + 1, z + 1) - 0.15f;
            s_glTexCoord2f(0, 0), s_glVertex3f(x, y00, z), s_glTexCoord2f(1, 0), s_glVertex3f(x + 1, y10, z), s_glTexCoord2f(1, 1), s_glVertex3f(x + 1, y11, z + 1);
            s_glTexCoord2f(0, 0), s_glVertex3f(x, y00, z), s_glTexCoord2f(1, 1), s_glVertex3f(x + 1, y11, z + 1), s_glTexCoord2f(0, 1), s_glVertex3f(x, y01, z + 1);
            if (prim_count > MAX_PRIMITIVE - 8) s_glEnd(), s_glBegin(GL_TRIANGLES);
        }
    s_glEnd();
    s_glEnable(GL_BLEND);
    s_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    s_glColor4ub(255, 255, 255, 170);
    bind(water1, 0);
    s_glBegin(GL_TRIANGLES);
    for (int z = z0; z < z1; z++)
        for (int x = x0; x < x1; x++) {
            if (!cell[z * mapw + x].holes) continue;
            float y = corner_y(x, z) - 0.05f;
            s_glTexCoord2f(0, 0), s_glVertex3f(x, y, z), s_glTexCoord2f(1, 0), s_glVertex3f(x + 1, y, z), s_glTexCoord2f(1, 1), s_glVertex3f(x + 1, y, z + 1);
            s_glTexCoord2f(0, 0), s_glVertex3f(x, y, z), s_glTexCoord2f(1, 1), s_glVertex3f(x + 1, y, z + 1), s_glTexCoord2f(0, 1), s_glVertex3f(x, y, z + 1);
            if (prim_count > MAX_PRIMITIVE - 8) s_glEnd(), s_glBegin(GL_TRIANGLES);
        }
    s_glEnd();
    s_glDisable(GL_BLEND);
    // the ground: the atlas, a cell a tile, its holes keyed out (the sea shows through)
    const Tex* a = &tex[terrain_tex];
    float su = (float)a->uw / a->w, sv = (float)a->uh / a->h;
    s_glColor4ub(255, 255, 255, 255);
    s_glEnable(GL_ALPHA_TEST);
    bind(terrain_tex, 10);
    s_glBegin(GL_TRIANGLES);
    for (int z = z0; z < z1; z++)
        for (int x = x0; x < x1; x++) {
            const Cell* c = &cell[z * mapw + x];
            float u0 = c->u0 * su, u1 = c->u1 * su, v0 = c->v0 * sv, v1 = c->v1 * sv;
            float y00 = corner_y(x, z), y10 = corner_y(x + 1, z), y01 = corner_y(x, z + 1), y11 = corner_y(x + 1, z + 1);
            s_glTexCoord2f(u0, v0), s_glVertex3f(x, y00, z), s_glTexCoord2f(u1, v0), s_glVertex3f(x + 1, y10, z), s_glTexCoord2f(u1, v1), s_glVertex3f(x + 1, y11, z + 1);
            s_glTexCoord2f(u0, v0), s_glVertex3f(x, y00, z), s_glTexCoord2f(u1, v1), s_glVertex3f(x + 1, y11, z + 1), s_glTexCoord2f(u0, v1), s_glVertex3f(x, y01, z + 1);
            if (prim_count > MAX_PRIMITIVE - 8) s_glEnd(), s_glBegin(GL_TRIANGLES);
        }
    s_glEnd();
    s_glDisable(GL_ALPHA_TEST);
}

// One mesh at (cx, cz) on the ground, turned by facing `face` (turret parts by `turret`); `pass` 0 solid and
// cutout, 1 the shadows and translucent faces, blended
static void draw_mesh(int mi, float cx, float cz, int face, int turret, int pass) {
    const Mesh* m = &mesh[mi];
    const float k = 1.0f / 1024;
    float y0 = ground_y(cx, cz);
    float a = face < 0 ? 0 : -face * (6.2831853f / 256), sa = sinf(a), ca = cosf(a);
    float ta = turret < 0 ? a : -turret * (6.2831853f / 256), st = sinf(ta), ct = cosf(ta);
    int cur = -2, curwrap = -1, part = 0;
    s_glBegin(GL_TRIANGLES);
    for (int t = 0; t < m->ntri; t++) {
        const Tri* r = &m->tri[t];
        while (part + 1 < m->nparts && t >= m->part_tri0[part + 1]) part++;
        int blended = r->mode == 2 || r->mode == 3;
        if (blended != pass) continue;
        if (r->ti != cur || (r->ti >= 0 && r->wrap != curwrap)) {
            s_glEnd();
            if (r->ti >= 0 && r->ti < ntex) bind(r->ti, r->wrap); else s_glDisable(GL_TEXTURE_2D);
            if (r->ti >= 0 && r->ti < ntex && (r->mode == 1 || tex[r->ti].cutout) && !pass) s_glEnable(GL_ALPHA_TEST);
            else s_glDisable(GL_ALPHA_TEST);
            cur = r->ti, curwrap = r->wrap;
            s_glBegin(GL_TRIANGLES);
        }
        int turreted = m->nparts > 1 && m->part_role[part] == 1 && turret >= 0;
        float s = turreted ? st : sa, c = turreted ? ct : ca;
        float px = turreted ? m->pivot[part * 3] : 0, pz = turreted ? m->pivot[part * 3 + 2] : 0;
        const Tex* tx = r->ti >= 0 && r->ti < ntex ? &tex[r->ti] : 0;
        for (int v = 0; v < 3; v++) {
            float x = r->v[v][0], y = r->v[v][1], z = r->v[v][2];
            if (turreted) {   // the turret about its pivot first, by its own facing less the body's
                float d = ta - a, sd = sinf(d), cd = cosf(d), dx = x - px, dz = z - pz;
                x = px + dx * cd - dz * sd, z = pz + dx * sd + dz * cd;
                s = sa, c = ca;
            }
            float wx = x * c - z * s, wz = x * s + z * c;
            if (tx) s_glTexCoord2f(r->v[v][3] * tx->uw / tx->w, r->v[v][4] * tx->uh / tx->h);
            s_glColor4ub(r->c[v][0], r->c[v][1], r->c[v][2], r->mode == 2 ? r->c[v][3] : 255);
            s_glVertex3f(cx + wx * k, y0 + y * k, cz + wz * k);
        }
        if (prim_count > MAX_PRIMITIVE - 8) s_glEnd(), s_glBegin(GL_TRIANGLES);
    }
    s_glEnd();
}

// A soldier: his set's STAND strip in his house's palette (pack's infantry table), the frame of his facing,
// a picture upright to the camera, 24 of its pixels a cell (the 1995 game's cell) and its foot on the ground
static int draw_soldier(const SE_Object* o, float cx, float cz) {
    const Inf* in = 0;
    for (int i = 0; i < ninf; i++)
        if (!strncmp(inf[i].code, o->type, 8)) in = &inf[i];
    if (!in) return 0;
    int si = in->strip[(o->house == 0 ? 0 : 1) * 3];
    if (si < 0 || si >= nsprite) return 0;
    const Sprite* s = &sprite[si];
    const Tex* t = &tex[s->ti];
    int stages = s->facings ? s->frames / s->facings : 1;
    int f = s->facings > 1 && o->face >= 0 ? ((o->face + 128 / s->facings) * s->facings / 256) % s->facings : 0;
    int frame = f * stages;
    float v0 = (float)(frame * s->fh) / t->h, v1 = (float)((frame + 1) * s->fh) / t->h, u1 = (float)s->fw / t->w;
    // upright in the view: up the screen is (0, cos p, -sin p) for a camera pitched down by p
    float hw = s->fw / 48.0f, hh = s->fh / 24.0f, p = pitch * 3.14159265f / 180, y = ground_y(cx, cz);
    float ty = y + hh * cosf(p), tz = cz - hh * sinf(p);
    bind(s->ti, 10);
    s_glEnable(GL_ALPHA_TEST);
    s_glColor4ub(255, 255, 255, 255);
    s_glBegin(GL_TRIANGLES);
    s_glTexCoord2f(0, v1), s_glVertex3f(cx - hw, y, cz);
    s_glTexCoord2f(u1, v1), s_glVertex3f(cx + hw, y, cz);
    s_glTexCoord2f(u1, v0), s_glVertex3f(cx + hw, ty, tz);
    s_glTexCoord2f(0, v1), s_glVertex3f(cx - hw, y, cz);
    s_glTexCoord2f(u1, v0), s_glVertex3f(cx + hw, ty, tz);
    s_glTexCoord2f(0, v0), s_glVertex3f(cx - hw, ty, tz);
    s_glEnd();
    s_glDisable(GL_ALPHA_TEST);
    return 1;
}

static SE_Object objects[1024];
static int drawn_objects, missing_meshes;

static void draw_frame(void) {
    s_glViewport(0, 0, win_w, win_h);
    s_glClearColor(0.11f, 0.2f, 0.29f, 1);
    s_glClear(GL_COLOR_BUFFER_BIT);
    s_glMatrixMode(GL_PROJECTION);
    s_glLoadIdentity();
    float zn = 0.1f * dist, zf = 2 * dist + 20, top = zn * tanf(25 * 3.14159265f / 180), right = top * win_w / win_h;
    s_glFrustum(-right, right, -top, top, zn, zf);
    s_glMatrixMode(GL_MODELVIEW);
    s_glLoadIdentity();
    s_glTranslatef(0, 0, -dist);
    s_glRotatef(pitch, 1, 0, 0);
    s_glTranslatef(-look_x, -ground_y(look_x, look_z), -look_z);
    draw_terrain();
    int n = ShaderEmu_Objects(objects, 1024);
    drawn_objects = missing_meshes = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass) s_glEnable(GL_BLEND), s_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (int i = 0; i < n; i++) {
            const SE_Object* o = &objects[i];
            float cx = o->x / 256.0f, cz = o->y / 256.0f;
            if (fabsf(cx - look_x) > 16 || cz < look_z - 20 || cz > look_z + 10) continue;
            if (o->kind == 0) {
                if (!pass) drawn_objects += draw_soldier(o, cx, cz), missing_meshes += !ninf;
                continue;
            }
            int mi = mesh_of(o->type);
            if (mi < 0 || mi >= nmesh) { missing_meshes += !pass; continue; }
            draw_mesh(mi, cx, cz, o->face, o->turret, pass);
            drawn_objects += !pass;
        }
        if (pass) s_glDisable(GL_BLEND);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 5) {
        fprintf(stderr, "opencnc3d CONTENT/ MISSIONS/ SCEN PACK [FRAMES]\n");
        return 2;
    }
    int frames = argc > 5 ? atoi(argv[5]) : 0;
    ShaderEmu_Set_Legacy_Render(0);
    unsigned i0 = instructions();
    if (!ShaderEmu_Start(argv[1], argv[2], argv[3], 1, 0)) {
        printf("opencnc3d: %s did not start\n", argv[3]);
        return 1;
    }
    unsigned i1 = instructions();
    printf("opencnc3d: started %s, %u instructions\n", argv[3], i1 - i0);
    const char* e = getenv("OPENCNC3D_TICKS");
    for (int t = e ? atoi(e) : 0; t > 0; t--) ShaderEmu_Advance();

    if (GrOpen() < 0) { printf("opencnc3d: no window system\n"); return 1; }
    GR_SCREEN_INFO info;
    GrGetScreenInfo(&info);
    if (win_w > info.cols) win_w = info.cols;
    if (win_h > info.rows - 55) win_h = info.rows - 55;
    GR_WINDOW_ID w = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, (char*)"OpenCNC3D", GR_ROOT_WINDOW_ID, -1, -1, win_w, win_h, 0);
    GrSelectEvents(w, GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
    GrMapWindow(w);
    GrSetFocus(w);
    if (!getenv("HLGL_WORKERS")) setenv("HLGL_WORKERS", "0", 0);
    if (!getenv("HL_POOL_MB")) setenv("HL_POOL_MB", "12", 0);
    if (hlglOpen(w) < 0) { printf("opencnc3d: no GPU\n"); return 1; }
    seglWorldUp(2);   // y
    seglSize(&win_w, &win_h);
    unsigned i2 = instructions();
    if (load_pack(argv[4]) < 0) { printf("opencnc3d: cannot read %s\n", argv[4]); return 1; }
    unsigned i3 = instructions(), tiles, in_gpu, gpu_size, left_out, verts;
    hlglStats(&tiles, &in_gpu, &gpu_size, &left_out, &verts);
    printf("opencnc3d: pack %s: %d textures, %d meshes, %d types, %u instructions; in RAM %u KB of meshes, cells and heights,"
           " in the GPU's pool %u of %u KB of textures (%u left out)\n", argv[4], ntex, nmesh, ntype, i3 - i2, pack_ram >> 10,
           in_gpu >> 10, gpu_size >> 10, left_out);

    // the camera on the player's MCV (GoodGuy's), else OPENCNC3D_LOOK, else the map's middle
    int n = ShaderEmu_Objects(objects, 1024);
    for (int i = 0; i < n; i++)
        if (!strcmp(objects[i].type, "MCV")) look_x = objects[i].x / 256.0f, look_z = objects[i].y / 256.0f;
    if ((e = getenv("OPENCNC3D_LOOK"))) sscanf(e, "%f,%f", &look_x, &look_z);
    if ((e = getenv("OPENCNC3D_DIST"))) dist = (float)atof(e);
    if ((e = getenv("OPENCNC3D_PITCH"))) pitch = (float)atof(e);

    unsigned logic = 0, draw = 0;
    for (int f = 0; frames <= 0 || f < frames; f++) {
        unsigned a = instructions();
        if (f) ShaderEmu_Advance();
        unsigned b = instructions();
        draw_frame();
        hlglSwap();
        unsigned c = instructions();
        logic += b - a, draw += c - b;
        if ((f + 1) % 30 == 0) {
            hlglStats(&tiles, &in_gpu, &gpu_size, &left_out, &verts);
            printf("opencnc3d: frames %d-%d: %u instructions a frame, logic %u, drawing %u; %d objects drawn, %d with no mesh, "
                   "%u vertices a frame\n", f - 28, f + 1, (logic + draw) / 30, logic / 30, draw / 30, drawn_objects,
                   missing_meshes, verts / 30);
            logic = draw = 0;
        }
        GR_EVENT ev;
        while (GrPeekEvent(&ev)) {
            GrGetNextEvent(&ev);
            if (ev.type == GR_EVENT_TYPE_CLOSE_REQ) return 0;
        }
    }
    printf("opencnc3d: done\n");
    return 0;
}
