/*
 * td_path: Tiberian Dawn's unit path search (vanilla-conquer tiberiandawn/findpath.cpp, condensed): Find_Path's
 * straight walk toward the goal by CELL_FACING, the zip through an obstacle to its far side, Follow_Edge round it
 * counterclockwise and clockwise (Next_Direction, the diagonal's corner test, Point_Relative_To_Line's side of the
 * start-to-goal line, Register_Cell's overlap bits) and the shorter detour taken. Unravel_Loop and Optimize_Moves
 * are left out: an edge walk that crosses its own trail fails. Passable_Cell reads the map's cells. The map: 64 x 64
 * cells with 90 blobs of rock, trees and buildings. A unit: 96 searches from the map's corners and edges to its
 * middle and across.
 */
#include <stdlib.h>
#include <string.h>
#include "wl.h"

#define MAP 64
#define CELLS (MAP * MAP)
#define MAX_MLIST_SIZE 300
#define MAX_PATH_EDGE_FOLLOW 400
#define SEARCHES 96
#define END ((signed char)-1)
typedef int CELL;
typedef int FacingType;   /* 0 north, 1 north-east ... 7 north-west; odd is diagonal (FACING_NE's bit) */
enum { COUNTERCLOCK = -1, CLOCK = 1 };
static const int dx8[8] = {0, 1, 1, 1, 0, -1, -1, -1}, dy8[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
static unsigned char map[CELLS];   /* 0 clear, 1 rough (passable, dearer), 2 blocked */
static CELL from[SEARCHES], to[SEARCHES];

typedef struct { CELL Start; int Cost, Length; signed char *Command; uint32_t *Overlap; } PathType;
struct finder { uint32_t MainOverlap[CELLS / 32], LeftOverlap[CELLS / 32], RightOverlap[CELLS / 32]; signed char moves[MAX_MLIST_SIZE + 2], left[MAX_MLIST_SIZE + 2], right[MAX_MLIST_SIZE + 2]; };

#define Cell_X(c) ((c) & (MAP - 1))
#define Cell_Y(c) ((c) >> 6)
static FacingType Next_Direction(FacingType facing, FacingType dir) { return (facing + dir) & 7; }
static CELL Adjacent_Cell(CELL c, FacingType f)
{
    int x = Cell_X(c) + dx8[f], y = Cell_Y(c) + dy8[f];
    if (x < 0 || y < 0 || x >= MAP || y >= MAP) return -1;
    return y * MAP + x;
}
/* CELL_FACING: the 8-way direction from one cell to another */
static FacingType Cell_Facing(CELL a, CELL b)
{
    int x = Cell_X(b) - Cell_X(a), y = Cell_Y(b) - Cell_Y(a), ax = abs(x), ay = abs(y);
    if (ax > 2 * ay) return x > 0 ? 2 : 6;
    if (ay > 2 * ax) return y > 0 ? 4 : 0;
    return x > 0 ? (y > 0 ? 3 : 1) : (y > 0 ? 5 : 7);
}
static int Passable_Cell(CELL cell)
{
    if (cell < 0) return 0;
    return map[cell] == 2 ? 0 : map[cell] == 1 ? 3 : 1;
}
static int Point_Relative_To_Line(int x, int z, int x1, int z1, int x2, int z2) { return (x - x2) * (z1 - z2) - (z - z2) * (x1 - x2); }

static int Register_Cell(PathType *path, CELL cell, FacingType dir, int cost)
{
    if (path->Overlap[cell >> 5] & (1u << (cell & 31))) return 0;
    path->Overlap[cell >> 5] |= 1u << (cell & 31);
    path->Command[path->Length++] = (signed char)dir;
    path->Command[path->Length] = END;
    path->Cost += cost;
    return 1;
}

static int Follow_Edge(CELL start, CELL target, PathType *path, int search, FacingType olddir, int max_cells)
{
    FacingType newdir, firstdir = -1;
    CELL oldcell = start, newcell, firstcell = -1;
    int cost = 0, startx = Cell_X(start), starty = Cell_Y(start), targetx = Cell_X(target), targety = Cell_Y(target);
    int online = 1, oldval = 0, cellcount = 0, forceout = 0;
    while (path->Length < max_cells) {
        newdir = olddir;
        for (;;) {
            int forcefail = 0;
            newdir = Next_Direction(newdir, search);
            if (newdir & 1) {
                CELL checkcell = Adjacent_Cell(oldcell, Next_Direction(newdir, search));
                if (checkcell == target && Passable_Cell(checkcell)) {
                    newdir = Next_Direction(newdir, search);
                    newcell = Adjacent_Cell(oldcell, newdir);
                    cost = Passable_Cell(newcell);
                    break;
                }
                checkcell = Adjacent_Cell(oldcell, newdir);
                if (checkcell >= 0) {
                    int checkval = Point_Relative_To_Line(Cell_X(checkcell), Cell_Y(checkcell), startx, starty, targetx, targety);
                    forcefail = (checkval && !online) ? ((checkval ^ oldval) < 0) : 0;
                    if (forcefail && path->Length > 0 && (FacingType)(newdir ^ 4) == path->Command[path->Length - 1]) forcefail = 0;
                }
            }
            if (newdir == olddir) return 0;
            newcell = Adjacent_Cell(oldcell, newdir);
            if (!forcefail && (cost = Passable_Cell(newcell)) != 0) break;
            if (newcell == target) {
                forceout = 1;
                break;
            }
        }
        if (!forceout) {
            if (!Register_Cell(path, newcell, newdir, cost)) return 0;   /* (Unravel_Loop left out) */
            int val = Point_Relative_To_Line(Cell_X(newcell), Cell_Y(newcell), startx, starty, targetx, targety);
            if (val) oldval = val, online = 0;
            else online = 1;
            if (++cellcount == MAX_PATH_EDGE_FOLLOW) return 0;
        }
        if (newcell == target) {
            path->Command[path->Length] = END;
            return 1;
        }
        if (newcell == firstcell && newdir == firstdir) return 0;
        if (firstcell == -1) firstcell = newcell, firstdir = newdir;
        olddir = Next_Direction(newdir, -search * 3);
        oldcell = newcell;
    }
    return 0;
}

static int Find_Path(struct finder *f, CELL source, CELL dest, PathType *path)
{
    CELL next, startcell = source;
    FacingType direction;
    int maxlen = MAX_MLIST_SIZE - 1;
    path->Start = source, path->Cost = 0, path->Length = 0, path->Command = f->moves, path->Command[0] = END;
    path->Overlap = f->MainOverlap;
    memset(path->Overlap, 0, sizeof f->MainOverlap);
    path->Overlap[source >> 5] |= 1u << (source & 31);
    while (path->Length < maxlen) {
        if (startcell == dest) break;
        direction = Cell_Facing(startcell, dest);
        next = Adjacent_Cell(startcell, direction);
        int cost = Passable_Cell(next);
        if (cost) {
            if (!Register_Cell(path, next, direction, cost)) break;
            startcell = next;
            continue;
        }
        if (next == dest) break;
        int found = 0;
        for (int limiter = 0; limiter < 5 && !found; limiter++) {
            for (;;) {   /* zip through the obstacle toward the goal */
                next = Adjacent_Cell(next, Cell_Facing(next, dest));
                if (next < 0) goto end_of_list;
                if (Passable_Cell(next) || next == dest) break;
            }
            PathType pleft = *path, pright = *path;
            pleft.Command = f->left, pleft.Overlap = f->LeftOverlap;
            memcpy(pleft.Command, path->Command, (size_t)path->Length);
            memcpy(pleft.Overlap, path->Overlap, sizeof f->LeftOverlap);
            int left = Follow_Edge(startcell, next, &pleft, COUNTERCLOCK, direction, MAX_MLIST_SIZE);
            pright.Command = f->right, pright.Overlap = f->RightOverlap;
            memcpy(pright.Command, path->Command, (size_t)path->Length);
            memcpy(pright.Overlap, path->Overlap, sizeof f->RightOverlap);
            int right = Follow_Edge(startcell, next, &pright, CLOCK, direction, MAX_MLIST_SIZE);
            PathType *which = 0;
            if (left && right) which = pleft.Cost < pright.Cost ? &pleft : &pright;
            else if (left) which = &pleft;
            else if (right) which = &pright;
            if (which) {
                memcpy(path->Command, which->Command, (size_t)which->Length + 1);
                memcpy(path->Overlap, which->Overlap, sizeof f->MainOverlap);
                path->Length = which->Length, path->Cost = which->Cost;
                startcell = next;
                found = 1;
            }
        }
        if (!found) break;
    }
end_of_list:
    path->Command[path->Length] = END;
    return startcell == dest;
}

static void setup(void)
{
    uint32_t seed = 0x7d47;
    for (int b = 0; b < 90; b++) {
        int cx = (int)(wl_rand(&seed) % MAP), cy = (int)(wl_rand(&seed) % MAP), w = 1 + (int)(wl_rand(&seed) % 5), h = 1 + (int)(wl_rand(&seed) % 4);
        unsigned char kind = (wl_rand(&seed) % 4) ? 2 : 1;
        for (int y = cy; y < cy + h && y < MAP; y++)
            for (int x = cx; x < cx + w && x < MAP; x++) map[y * MAP + x] = kind;
    }
    for (int s = 0; s < SEARCHES; s++) {
        CELL a, b;
        do a = (CELL)(wl_rand(&seed) % CELLS);
        while (map[a] == 2 || (Cell_X(a) > 8 && Cell_X(a) < MAP - 8 && Cell_Y(a) > 8 && Cell_Y(a) < MAP - 8));
        do b = (CELL)(wl_rand(&seed) % CELLS);
        while (map[b] == 2 || abs(Cell_X(b) - Cell_X(a)) + abs(Cell_Y(b) - Cell_Y(a)) < 40);
        from[s] = a, to[s] = b;
    }
}

static void *inst(void) { return wl_alloc(sizeof(struct finder)); }

static uint32_t run(void *ctx)
{
    struct finder *f = ctx;
    uint32_t sum = 0;
    for (int s = 0; s < SEARCHES; s++) {
        PathType path;
        int ok = Find_Path(f, from[s], to[s], &path);
        sum = sum * 31 + (uint32_t)path.Length * 2 + (uint32_t)ok + (uint32_t)path.Cost * 977;
    }
    return sum;
}

const wl_kernel wl_td_path = {"td_path", "Tiberian Dawn: Find_Path's straight walk and Follow_Edge round obstacles", setup, inst, run};
