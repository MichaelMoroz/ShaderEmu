/*
 * cc_mesh: ClassiCube building a chunk's mesh, what it does whenever a chunk comes into view or a block changes
 * (Builder.c, copied as the port has it): ReadChunkBody (the chunk and a block's border out of the world,
 * all-air and all-solid noted), PrepareChunk's six faces a block (hidden faces by the Blocks.Hidden table, the
 * port's skip of a block buried among six opaque ones), NormalBuilder_StretchX/StretchZ merging a run of equal
 * faces with Normal_CanStretch's light test, and NormalBuilder_RenderBlock writing each face's four vertices
 * (Drawer_XMin and the rest: place, colour, texture). The world: 64 x 64 x 64 blocks of hills (stone, dirt,
 * grass, sand, water, some leaves and flowers) with a height map for the light. A unit: 4 chunks of 16^3, two
 * at the surface, one under, one in the air over a hill.
 */
#include <string.h>
#include "wl.h"

#define WORLD 64
#define CHUNK_SIZE 16
#define EXTCHUNK_SIZE 18
#define EXTCHUNK_SIZE_2 (EXTCHUNK_SIZE * EXTCHUNK_SIZE)
#define EXTCHUNK_SIZE_3 (EXTCHUNK_SIZE_2 * EXTCHUNK_SIZE)
#define CHUNK_SIZE_3 (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)
#define FACE_COUNT 6
enum { FACE_XMIN, FACE_XMAX, FACE_ZMIN, FACE_ZMAX, FACE_YMIN, FACE_YMAX };
enum { BLOCK_AIR, BLOCK_STONE, BLOCK_GRASS, BLOCK_DIRT, BLOCK_WATER = 8, BLOCK_STILL_WATER, BLOCK_LAVA, BLOCK_STILL_LAVA, BLOCK_SAND,
       BLOCK_LEAVES = 18, BLOCK_DANDELION = 37, BLOCK_COUNT = 64 };
enum { DRAW_OPAQUE, DRAW_TRANSPARENT, DRAW_TRANSLUCENT, DRAW_GAS, DRAW_SPRITE };
#define Builder_PackChunk(xx, yy, zz) (((yy) + 1) * EXTCHUNK_SIZE_2 + ((zz) + 1) * EXTCHUNK_SIZE + ((xx) + 1))
#define Builder_PackCount(xx, yy, zz) ((((yy) << 8) | ((zz) << 4) | (xx)) * FACE_COUNT)

typedef unsigned char BlockID;
struct VertexTextured { float x, y, z; uint32_t col; float u, v; };
static struct {
    unsigned char Draw[BLOCK_COUNT], FullOpaque[BLOCK_COUNT], Brightness[BLOCK_COUNT], CanStretch[BLOCK_COUNT], LightOffset[BLOCK_COUNT];
    unsigned char Hidden[BLOCK_COUNT * BLOCK_COUNT];
    unsigned short Textures[BLOCK_COUNT * FACE_COUNT];
} Blocks;
static BlockID world[WORLD * WORLD * WORLD];
static short heightmap[WORLD * WORLD];
static const int Builder_Offsets[FACE_COUNT] = {-1, 1, -EXTCHUNK_SIZE, EXTCHUNK_SIZE, -EXTCHUNK_SIZE_2, EXTCHUNK_SIZE_2};
static const int chunks[4][3] = {{16, 16, 16}, {32, 16, 0}, {0, 0, 32}, {48, 32, 16}};
#define World_Pack(x, y, z) (((y) * WORLD + (z)) * WORLD + (x))
#define MAX_VERTS 7000   /* (the most a chunk here makes: 6,516) */

struct builder {
    BlockID chunk[EXTCHUNK_SIZE_3];
    unsigned char counts[CHUNK_SIZE_3 * FACE_COUNT];
    struct VertexTextured verts[MAX_VERTS];
    int nverts, X, Y, Z, fullBright, endX, endZ;
};

static uint32_t Normal_LightColor(int x, int y, int z, int face)
{
    int lit = y > heightmap[z * WORLD + x];
    static const uint32_t sun[FACE_COUNT] = {0xff999999, 0xff999999, 0xffcccccc, 0xffcccccc, 0xff7f7f7f, 0xffffffff};
    return lit ? sun[face] : (sun[face] >> 1) & 0xff7f7f7f;
}

static int Normal_CanStretch(struct builder *b, BlockID initial, int chunkIndex, int x, int y, int z, int face)
{
    BlockID cur = b->chunk[chunkIndex];
    if (cur != initial || (Blocks.Hidden[cur * BLOCK_COUNT + b->chunk[chunkIndex + Builder_Offsets[face]]] & (1 << face))) return 0;
    if (b->fullBright) return 1;
    return Normal_LightColor(b->X, b->Y, b->Z, face) == Normal_LightColor(x, y, z, face);
}

static int StretchX(struct builder *b, int countIndex, int x, int y, int z, int chunkIndex, BlockID block, int face)
{
    int count = 1, stretchTile;
    x++, chunkIndex++, countIndex += FACE_COUNT;
    stretchTile = (Blocks.CanStretch[block] & (1 << face)) != 0;
    while (x < b->endX && stretchTile && Normal_CanStretch(b, block, chunkIndex, x, y, z, face)) {
        b->counts[countIndex] = 0;
        count++, x++, chunkIndex++, countIndex += FACE_COUNT;
    }
    return count;
}

static int StretchZ(struct builder *b, int countIndex, int x, int y, int z, int chunkIndex, BlockID block, int face)
{
    int count = 1, stretchTile;
    z++, chunkIndex += EXTCHUNK_SIZE, countIndex += CHUNK_SIZE * FACE_COUNT;
    stretchTile = (Blocks.CanStretch[block] & (1 << face)) != 0;
    while (z < b->endZ && stretchTile && Normal_CanStretch(b, block, chunkIndex, x, y, z, face)) {
        b->counts[countIndex] = 0;
        count++, z++, chunkIndex += EXTCHUNK_SIZE, countIndex += CHUNK_SIZE * FACE_COUNT;
    }
    return count;
}

/* Drawer_XMin and the rest: a face's four corners, its texture's tile in the atlas and its light */
static void Drawer_Face(struct builder *b, int face, int count, uint32_t col, int loc, float x1, float y1, float z1)
{
    static const float corner[FACE_COUNT][4][3] = {
        {{0, 1, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 1}}, {{1, 1, 1}, {1, 0, 1}, {1, 0, 0}, {1, 1, 0}}, {{1, 1, 0}, {1, 0, 0}, {0, 0, 0}, {0, 1, 0}},
        {{0, 1, 1}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}}, {{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}}, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}};
    if (b->nverts + 4 > MAX_VERTS) return;
    float u1 = (loc & 15) * (1.0f / 16), v1 = (loc >> 4) * (1.0f / 16), u2 = u1 + count * (15.99f / 256), v2 = v1 + 15.99f / 256;
    int along = face < FACE_ZMIN ? 2 : 0;   /* a run is along z for the x faces, along x for the others */
    for (int k = 0; k < 4; k++) {
        struct VertexTextured *v = &b->verts[b->nverts++];
        float c[3] = {corner[face][k][0], corner[face][k][1], corner[face][k][2]};
        c[along] *= (float)count;
        v->x = x1 + c[0], v->y = y1 + c[1], v->z = z1 + c[2], v->col = col;
        v->u = (k == 0 || k == 1) ? u1 : u2, v->v = (k == 1 || k == 2) ? v2 : v1;
    }
}

static void RenderBlock(struct builder *b, int index, int x, int y, int z, BlockID block)
{
    int lightFlags = Blocks.LightOffset[block], fullBright = Blocks.Brightness[block];
    for (int face = 0; face < FACE_COUNT; face++) {
        int count = b->counts[index + face];
        if (!count) continue;
        int offset = (lightFlags >> face) & 1, lx = x, ly = y, lz = z;
        if (face == FACE_XMIN) lx -= offset;
        else if (face == FACE_XMAX) lx += offset;
        else if (face == FACE_ZMIN) lz -= offset;
        else if (face == FACE_ZMAX) lz += offset;
        else if (face == FACE_YMIN) ly -= offset;
        else ly += offset;
        if (lx < 0) lx = 0;
        if (lx >= WORLD) lx = WORLD - 1;
        if (lz < 0) lz = 0;
        if (lz >= WORLD) lz = WORLD - 1;
        uint32_t col = fullBright ? 0xffffffff : Normal_LightColor(lx, ly, lz, face);
        Drawer_Face(b, face, count, col, Blocks.Textures[block * FACE_COUNT + face], (float)x, (float)y, (float)z);
    }
}

static void make_chunk(struct builder *b, int x1, int y1, int z1)
{
    int allAir = 1, allSolid = 1, x, y, z, xx, yy, zz, index, cIndex;
    BlockID block;
    for (yy = -1; yy < 17; ++yy) {   /* ReadChunkBody */
        y = yy + y1;
        for (zz = -1; zz < 17; ++zz) {
            z = z1 + zz;
            cIndex = Builder_PackChunk(-1, yy, zz);
            for (xx = -1; xx < 17; ++xx, ++cIndex) {
                x = x1 + xx;
                block = (x < 0 || y < 0 || z < 0 || x >= WORLD || y >= WORLD || z >= WORLD) ? (y < 0 ? BLOCK_STONE : BLOCK_AIR) : world[World_Pack(x, y, z)];
                allAir = allAir && Blocks.Draw[block] == DRAW_GAS;
                allSolid = allSolid && Blocks.FullOpaque[block];
                b->chunk[cIndex] = block;
            }
        }
    }
    if (allAir || allSolid) return;
    memset(b->counts, 1, sizeof b->counts);
    b->endX = x1 + CHUNK_SIZE, b->endZ = z1 + CHUNK_SIZE;
    for (y = y1, yy = 0; y < y1 + CHUNK_SIZE; y++, yy++) {   /* PrepareChunk */
        for (z = z1, zz = 0; z < z1 + CHUNK_SIZE; z++, zz++) {
            cIndex = Builder_PackChunk(0, yy, zz);
            for (x = x1, xx = 0; x < x1 + CHUNK_SIZE; x++, xx++, cIndex++) {
                BlockID bk = b->chunk[cIndex];
                if (Blocks.Draw[bk] == DRAW_GAS) continue;
                index = Builder_PackCount(xx, yy, zz);
                if (Blocks.Draw[bk] == DRAW_SPRITE) continue;
                if (Blocks.FullOpaque[bk] && Blocks.FullOpaque[b->chunk[cIndex - 1]] && Blocks.FullOpaque[b->chunk[cIndex + 1]] &&
                    Blocks.FullOpaque[b->chunk[cIndex - EXTCHUNK_SIZE]] && Blocks.FullOpaque[b->chunk[cIndex + EXTCHUNK_SIZE]] &&
                    Blocks.FullOpaque[b->chunk[cIndex - EXTCHUNK_SIZE_2]] && Blocks.FullOpaque[b->chunk[cIndex + EXTCHUNK_SIZE_2]]) {
                    memset(&b->counts[index], 0, FACE_COUNT);
                    continue;
                }
                b->X = x, b->Y = y, b->Z = z;
                b->fullBright = Blocks.Brightness[bk];
                int tileIdx = bk * BLOCK_COUNT;
                if (b->counts[index] == 0 || (x == 0 && y < 32) || (x != 0 && (Blocks.Hidden[tileIdx + b->chunk[cIndex - 1]] & (1 << FACE_XMIN))))
                    b->counts[index] = 0;
                else b->counts[index] = (unsigned char)StretchZ(b, index, x, y, z, cIndex, bk, FACE_XMIN);
                index++;
                if (b->counts[index] == 0 || (x == WORLD - 1 && y < 32) || (x != WORLD - 1 && (Blocks.Hidden[tileIdx + b->chunk[cIndex + 1]] & (1 << FACE_XMAX))))
                    b->counts[index] = 0;
                else b->counts[index] = (unsigned char)StretchZ(b, index, x, y, z, cIndex, bk, FACE_XMAX);
                index++;
                if (b->counts[index] == 0 || (z == 0 && y < 32) || (z != 0 && (Blocks.Hidden[tileIdx + b->chunk[cIndex - EXTCHUNK_SIZE]] & (1 << FACE_ZMIN))))
                    b->counts[index] = 0;
                else b->counts[index] = (unsigned char)StretchX(b, index, x, y, z, cIndex, bk, FACE_ZMIN);
                index++;
                if (b->counts[index] == 0 || (z == WORLD - 1 && y < 32) ||
                    (z != WORLD - 1 && (Blocks.Hidden[tileIdx + b->chunk[cIndex + EXTCHUNK_SIZE]] & (1 << FACE_ZMAX))))
                    b->counts[index] = 0;
                else b->counts[index] = (unsigned char)StretchX(b, index, x, y, z, cIndex, bk, FACE_ZMAX);
                index++;
                if (b->counts[index] == 0 || y == 0 || (Blocks.Hidden[tileIdx + b->chunk[cIndex - EXTCHUNK_SIZE_2]] & (1 << FACE_YMIN))) b->counts[index] = 0;
                else b->counts[index] = (unsigned char)StretchX(b, index, x, y, z, cIndex, bk, FACE_YMIN);
                index++;
                if (b->counts[index] == 0 || (Blocks.Hidden[tileIdx + b->chunk[cIndex + EXTCHUNK_SIZE_2]] & (1 << FACE_YMAX))) b->counts[index] = 0;
                else b->counts[index] = (unsigned char)StretchX(b, index, x, y, z, cIndex, bk, FACE_YMAX);
            }
        }
    }
    for (y = y1, yy = 0; y < y1 + CHUNK_SIZE; y++, yy++)   /* the vertices, block by block */
        for (z = z1, zz = 0; z < z1 + CHUNK_SIZE; z++, zz++) {
            cIndex = Builder_PackChunk(0, yy, zz);
            for (x = x1, xx = 0; x < x1 + CHUNK_SIZE; x++, xx++, cIndex++) {
                BlockID bk = b->chunk[cIndex];
                if (Blocks.Draw[bk] == DRAW_GAS || Blocks.Draw[bk] == DRAW_SPRITE) continue;
                RenderBlock(b, Builder_PackCount(xx, yy, zz), x, y, z, bk);
            }
        }
}

static void setup(void)
{
    uint32_t seed = 0xc1a5;
    for (int i = 0; i < BLOCK_COUNT; i++) {
        Blocks.Draw[i] = i == BLOCK_AIR ? DRAW_GAS : i == BLOCK_DANDELION ? DRAW_SPRITE : (i == BLOCK_WATER || i == BLOCK_STILL_WATER) ? DRAW_TRANSLUCENT
                         : i == BLOCK_LEAVES ? DRAW_TRANSPARENT : DRAW_OPAQUE;
        Blocks.FullOpaque[i] = Blocks.Draw[i] == DRAW_OPAQUE;
        Blocks.Brightness[i] = i == BLOCK_LAVA || i == BLOCK_STILL_LAVA;
        Blocks.CanStretch[i] = i == BLOCK_LEAVES ? 0 : 0x3f;
        Blocks.LightOffset[i] = Blocks.FullOpaque[i] ? 0x3f : 0;
        for (int f = 0; f < FACE_COUNT; f++) Blocks.Textures[i * FACE_COUNT + f] = (unsigned short)((i * 3 + (f == FACE_YMAX ? 0 : f == FACE_YMIN ? 2 : 1)) & 255);
    }
    for (int a = 0; a < BLOCK_COUNT; a++)   /* Block_CalcCulling: a face is hidden by an opaque neighbour, or by the same see-through block */
        for (int o = 0; o < BLOCK_COUNT; o++) {
            unsigned char h = 0;
            if (Blocks.FullOpaque[o]) h = 0x3f;
            else if (a == o && Blocks.Draw[a] != DRAW_OPAQUE && Blocks.Draw[a] != DRAW_GAS) h = 0x3f;
            else if (Blocks.Draw[a] == DRAW_TRANSLUCENT && Blocks.Draw[o] == DRAW_TRANSLUCENT) h = 0x3f;
            Blocks.Hidden[a * BLOCK_COUNT + o] = h;
        }
    for (int z = 0; z < WORLD; z++)
        for (int x = 0; x < WORLD; x++) {
            int h = 24 + (int)(8 * ((x * 7 + z * 3) % 23) / 23.0f) + (int)(6 * ((x * x + z * 5) % 17) / 17.0f) + (int)(wl_rand(&seed) % 3);
            heightmap[z * WORLD + x] = (short)h;
            for (int y = 0; y < WORLD; y++) {
                BlockID b = BLOCK_AIR;
                if (y < h - 4) b = BLOCK_STONE;
                else if (y < h) b = BLOCK_DIRT;
                else if (y == h) b = h < 28 ? BLOCK_SAND : BLOCK_GRASS;
                else if (y <= 28) b = BLOCK_STILL_WATER;
                else if (y == h + 1 && wl_rand(&seed) % 13 == 0) b = BLOCK_DANDELION;
                if (b == BLOCK_STONE && wl_rand(&seed) % 40 == 0) b = BLOCK_AIR;   /* caves' holes */
                world[World_Pack(x, y, z)] = b;
            }
            if (wl_rand(&seed) % 31 == 0)   /* a tree's leaves */
                for (int y = h + 3; y < h + 7 && y < WORLD; y++)
                    for (int dz = -2; dz <= 2; dz++)
                        for (int dx = -2; dx <= 2; dx++)
                            if (x + dx >= 0 && x + dx < WORLD && z + dz >= 0 && z + dz < WORLD) world[World_Pack(x + dx, y, z + dz)] = BLOCK_LEAVES;
        }
}

static void *inst(void) { return wl_alloc(sizeof(struct builder)); }

static uint32_t run(void *ctx)
{
    struct builder *b = ctx;
    uint32_t sum = 0;
    for (int c = 0; c < 4; c++) {
        b->nverts = 0;
        make_chunk(b, chunks[c][0], chunks[c][1], chunks[c][2]);
        sum = sum * 31 + (uint32_t)b->nverts;
        for (int i = 0; i < b->nverts; i += 13) sum += (uint32_t)(int)(b->verts[i].x * 4 + b->verts[i].y * 64 + b->verts[i].z * 1024) ^ b->verts[i].col;
    }
    return sum;
}

const wl_kernel wl_cc_mesh = {"cc_mesh", "ClassiCube: a chunk's mesh, PrepareChunk, StretchX/Z and the faces' vertices", setup, inst, run};
