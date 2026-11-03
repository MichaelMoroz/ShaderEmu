// SPDX-License-Identifier: GPL-3.0-or-later
// ShaderEmu: what linux/opencnc3d/host.cpp needs of OpenCNC3D's brain, compiled into it
// (docs/games/opencnc3d.md). The brain's own interface is the CNC_* functions of
// dllinterface.cpp; this starts a mission through them with a fixed seed and gives the
// game's state as a sum, walking the engine's heaps the way OpenCNC3D_Dump_Objects does.
#include "opencnc3d_compat.h"
#include "function.h"
#include "externs.h"
#include "dllinterface.h"
#include <stdio.h>
#include <string.h>

typedef void(__cdecl* SE_Event_Callback)(const EventCallbackStruct& event);
extern "C" void __cdecl CNC_Init(const char* command_line, SE_Event_Callback event_callback);
extern "C" void __cdecl CNC_Config(const CNCRulesDataStruct& rules);
extern "C" bool __cdecl CNC_Start_Custom_Instance(const char* content_directory,
                                                  const char* directory_path,
                                                  const char* scenario_name,
                                                  int build_level,
                                                  bool multiplayer);
extern "C" bool __cdecl CNC_Advance_Instance(unsigned long long player_id);
extern "C" int __cdecl OpenCNC3D_Dump_Objects(void);

// Whether CNC_Advance_Instance draws the 1995 view into the hidden page (our patch to
// DLLExportClass::Legacy_Render_Enabled): on, as the library has it, unless the host says no.
bool ShaderEmu_Legacy_Render = true;
extern "C" void ShaderEmu_Set_Legacy_Render(int on)
{
    ShaderEmu_Legacy_Render = on != 0;
}

// MapClass::Clean on the marked cells (our patch to map.cpp): 0 those, 1 every cell as the
// Remaster has it, 2 those and a count of the cells it would have missed (must stay 0).
extern int ShaderEmu_Clean_Mode;
extern int ShaderEmu_Clean_Missed;
extern "C" void ShaderEmu_Set_Clean(int mode)
{
    ShaderEmu_Clean_Mode = mode;
}
extern "C" int ShaderEmu_Clean_Missed_Count(void)
{
    return ShaderEmu_Clean_Missed;
}

// The dumps the renderer reads back (OpenCNC3D_Dump_Objects, OpenCNC3D_Dump_Selection) print through this,
// our patch's `#define printf` over them in dllinterface.cpp. Outside a capture it is vprintf. While the renderer
// captures (ShaderEmu_Dump_On, set by cnc_eyes.cpp's capture_brain_call in our patch) a line goes into
// ShaderEmu_Dump_Buf, formatted here: the lines have only %d and %s, which musl's printf_core made at 30
// thousand instructions an object, and the stdout it went to was a scratch file read back through the kernel
// (docs/games/opencnc3d.md, "The object dump"). Any other conversion is vsnprintf's. OPENCNC3D_DUMP=check
// formats every line both ways too and counts the lines that differ (DUMPCHECK| at exit).
#include <stdarg.h>
#include <stdlib.h>
extern "C" {
char* ShaderEmu_Dump_Buf;
unsigned ShaderEmu_Dump_Len, ShaderEmu_Dump_Cap;
int ShaderEmu_Dump_On;
}
#include "se_dump.h"
#define SE_OBJ_NAME(n) #n,
extern "C" const char* const ShaderEmu_Obj_Names[SE_OBJ_COUNT] = { SE_OBJ_FIELDS(SE_OBJ_NAME) };
extern "C" {
ShaderEmu_ObjRec* ShaderEmu_Obj_Recs;
unsigned ShaderEmu_Obj_Count;
}
static unsigned obj_cap;
// the next record of the dump being captured (the renderer sets ShaderEmu_Obj_Count to 0 before it), or 0
extern "C" ShaderEmu_ObjRec* ShaderEmu_Obj_Next(void)
{
    if (ShaderEmu_Obj_Count == obj_cap) {
        unsigned cap = obj_cap ? obj_cap * 2 : 256;
        ShaderEmu_ObjRec* r = (ShaderEmu_ObjRec*)realloc(ShaderEmu_Obj_Recs, cap * sizeof(ShaderEmu_ObjRec));
        if (!r)
            return 0;
        ShaderEmu_Obj_Recs = r;
        obj_cap = cap;
    }
    return &ShaderEmu_Obj_Recs[ShaderEmu_Obj_Count++];
}
// The cells OpenCNC3D_Dump_Objects's five cell walks print anything of (an overlay: tiberium, crates, fields,
// walls; a smudge), ascending, as each walk went: those walks went over the 16,384 cells of the MEGAMAPS grid
// five times a tick, 1.02 million instructions. A legacy map's cells lie in the grid's top-left 64 x 64 (the
// engine confines it there), so only those are looked at for one; OPENCNC3D_DUMP=check looks at the whole grid
// as well and says how many cells outside had either (DUMPCELLS|, at exit: must be 0).
static int dump_check = -1;
static unsigned long cells_outside;
static void dump_cells_report(void)
{
    fprintf(stderr, "DUMPCELLS|outside=%lu\n", cells_outside);
}
extern "C" int ShaderEmu_Dump_Cells(int* cells)
{
    const int side = Map.MapBinaryVersion == MAP_VERSION_MEGA ? MAP_CELL_W : 64;
    const int rows = Map.MapBinaryVersion == MAP_VERSION_MEGA ? MAP_CELL_H : 64;
    int n = 0;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < side; x++) {
            const int cell = y * MAP_CELL_W + x;
            const CellClass& c = Map[(CELL)cell];
            if (c.Overlay != OVERLAY_NONE || c.Smudge != SMUDGE_NONE)
                cells[n++] = cell;
        }
    if (dump_check > 0 && side < MAP_CELL_W) {
        static bool told;
        if (!told) {
            told = true;
            atexit(dump_cells_report);
        }
        for (int cell = 0; cell < MAP_CELL_TOTAL; cell++) {
            const CellClass& c = Map[(CELL)cell];
            if ((c.Overlay != OVERLAY_NONE || c.Smudge != SMUDGE_NONE) && (Cell_X(cell) >= side || Cell_Y(cell) >= rows))
                cells_outside++;
        }
    }
    return n;
}
static unsigned long dump_lines, dump_bad;
static void dump_check_report(void)
{
    fprintf(stderr, "DUMPCHECK|lines=%lu|differ=%lu\n", dump_lines, dump_bad);
}
static bool dump_grow(unsigned more);
static inline bool dump_room(unsigned more)
{
    return ShaderEmu_Dump_Len + more <= ShaderEmu_Dump_Cap || dump_grow(more);
}
static bool dump_grow(unsigned more)
{
    unsigned cap = ShaderEmu_Dump_Cap ? ShaderEmu_Dump_Cap : 1 << 16;
    while (cap < ShaderEmu_Dump_Len + more)
        cap *= 2;
    char* b = (char*)realloc(ShaderEmu_Dump_Buf, cap);
    if (!b)
        return false;
    ShaderEmu_Dump_Buf = b;
    ShaderEmu_Dump_Cap = cap;
    return true;
}
extern "C" int ShaderEmu_Dump_Printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (!ShaderEmu_Dump_On) {
        int r = vprintf(fmt, ap);
        va_end(ap);
        return r;
    }
    if (dump_check < 0) {
        const char* e = getenv("OPENCNC3D_DUMP");
        dump_check = e && !strcmp(e, "check");
        if (dump_check)
            atexit(dump_check_report);
    }
    va_list again;
    va_copy(again, ap);
    const unsigned start = ShaderEmu_Dump_Len;
    bool slow = !dump_room(4096);
    // room for a run of the format's text (none is near 4 KB) or a number, made again at each conversion
    for (const char* f = fmt; *f && !slow; f++) {
        if (*f != '%') {
            ShaderEmu_Dump_Buf[ShaderEmu_Dump_Len++] = *f;
            continue;
        }
        if (!dump_room(4096)) {
            slow = true;
            break;
        }
        char* o = ShaderEmu_Dump_Buf + ShaderEmu_Dump_Len;
        switch (f[1]) {
        case 'd': {
            int v = va_arg(ap, int);
            unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
            char t[12];
            int n = 0;
            do {
                t[n++] = (char)('0' + u % 10);
                u /= 10;
            } while (u);
            if (v < 0)
                *o++ = '-';
            while (n)
                *o++ = t[--n];
            ShaderEmu_Dump_Len = (unsigned)(o - ShaderEmu_Dump_Buf);
            f++;
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s)
                s = "(null)";
            unsigned n = (unsigned)strlen(s);
            if (!dump_room(n + 4096)) {
                slow = true;
                break;
            }
            memcpy(ShaderEmu_Dump_Buf + ShaderEmu_Dump_Len, s, n);
            ShaderEmu_Dump_Len += n;
            f++;
            break;
        }
        case '%':
            *o = '%';
            ShaderEmu_Dump_Len++;
            f++;
            break;
        default:
            slow = true;
            break;
        }
    }
    int r;
    if (slow) {
        // a conversion of another kind: the line again, by the library
        ShaderEmu_Dump_Len = start;
        va_list third;
        va_copy(third, again);
        int n = vsnprintf(NULL, 0, fmt, again);
        if (n > 0 && dump_room((unsigned)n + 1)) {
            vsnprintf(ShaderEmu_Dump_Buf + start, (size_t)n + 1, fmt, third);
            ShaderEmu_Dump_Len = start + (unsigned)n;
        }
        va_end(third);
        r = n;
    } else {
        r = (int)(ShaderEmu_Dump_Len - start);
        if (dump_check > 0) {
            char ref[1024];
            int n = vsnprintf(ref, sizeof ref, fmt, again);
            dump_lines++;
            if (n >= (int)sizeof ref ? n != r
                                     : n != r || memcmp(ref, ShaderEmu_Dump_Buf + start, (size_t)r) != 0) {
                dump_bad++;
                fprintf(stderr, "DUMPCHECK|DIFFER|%.*s", n, ref);
            }
        }
    }
    va_end(again);
    va_end(ap);
    return r;
}

static int se_events;
static void __cdecl se_event(const EventCallbackStruct& e)
{
    se_events++;
    if (e.EventType == CALLBACK_EVENT_GAME_OVER)
        printf("ocnc: game over\n");
}

// The brain with the rules OpenCNC3D's brain/host/run1.cpp gives it, and a mission. The game's
// random numbers start from RandomClass(0) whatever the clock says (Clear_Scenario, scenario.cpp),
// so a mission plays the same every time: seed 0 is that, the game as OpenCNC3D plays it. Another
// seed starts both generators there once the mission is loaded.
extern "C" int ShaderEmu_Start(const char* content, const char* dir, const char* scen, int build_level,
                               int seed)
{
    CNC_Init("", se_event);
    CNCRulesDataStruct rules;
    memset(&rules, 0, sizeof(rules));
    for (int i = 0; i < 3; i++) {
        rules.Difficulties[i].FirepowerBias = 1.0f;
        rules.Difficulties[i].GroundspeedBias = 1.0f;
        rules.Difficulties[i].AirspeedBias = 1.0f;
        rules.Difficulties[i].ArmorBias = 1.0f;
        rules.Difficulties[i].ROFBias = 1.0f;
        rules.Difficulties[i].CostBias = 1.0f;
        rules.Difficulties[i].BuildSpeedBias = 1.0f;
        rules.Difficulties[i].RepairDelay = 0.02f;
        rules.Difficulties[i].BuildDelay = 0.03f;
        rules.Difficulties[i].IsBuildSlowdown = false;
        rules.Difficulties[i].IsWallDestroyer = true;
        rules.Difficulties[i].IsContentScan = true;
    }
    CNC_Config(rules);
    if (!CNC_Start_Custom_Instance(content, dir, scen, build_level, false))
        return 0;
    if (seed) {
        Scen.RandomNumber = RandomClass(seed);
        NonCriticalRandomNumber = RandomClass(seed);
    }
    return 1;
}

extern "C" int ShaderEmu_Advance(void)
{
    return CNC_Advance_Instance(0) ? 1 : 0;
}

extern "C" int ShaderEmu_Dump(void)
{
    return OpenCNC3D_Dump_Objects();
}

extern "C" int ShaderEmu_Frame(void)
{
    return (int)Frame;
}

// FNV-1a over 32-bit words: the same on the build computer and the machine, whatever the
// size of a pointer, since only the game's numbers go in.
static unsigned se_h;
static void mix(unsigned v)
{
    for (int i = 0; i < 4; i++) {
        se_h ^= (v >> (8 * i)) & 0xFF;
        se_h *= 16777619u;
    }
}
static void mix_name(const char* s)
{
    while (s && *s) {
        se_h ^= (unsigned char)*s++;
        se_h *= 16777619u;
    }
    mix(0);
}

static void mix_object(ObjectClass* o, int heapid)
{
    if (o == NULL || !o->IsActive)
        return;
    mix(heapid);
    mix((unsigned)o->What_Am_I());
    mix_name(o->Class_Of().IniName);
    mix((unsigned)o->Owner());
    mix((unsigned)o->Coord);
    mix((unsigned)o->Strength);
    mix(o->IsInLimbo ? 1 : 0);
    if (o->Is_Techno()) {
        TechnoClass* t = (TechnoClass*)o;
        mix((unsigned)t->Get_Mission());
        mix((unsigned)t->TarCom);
        mix((unsigned)t->PrimaryFacing.Current());
        RTTIType r = o->What_Am_I();
        if (r == RTTI_INFANTRY || r == RTTI_UNIT || r == RTTI_AIRCRAFT)
            mix((unsigned)((FootClass*)o)->NavCom);
    }
}

// counts: infantry, units, buildings, aircraft, terrains, bullets, anims (live objects)
extern "C" unsigned ShaderEmu_State_Sum(int* counts)
{
    se_h = 2166136261u;
    mix((unsigned)Frame);
    mix(Scen.RandomNumber.Seed);
    mix(NonCriticalRandomNumber.Seed);
    int n[7] = {0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < Infantry.Count(); i++) {
        InfantryClass* p = Infantry.Ptr(i);
        n[0] += p && p->IsActive;
        mix_object(p, Infantry.ID(p));
    }
    for (int i = 0; i < Units.Count(); i++) {
        UnitClass* p = Units.Ptr(i);
        n[1] += p && p->IsActive;
        mix_object(p, Units.ID(p));
    }
    for (int i = 0; i < Buildings.Count(); i++) {
        BuildingClass* p = Buildings.Ptr(i);
        n[2] += p && p->IsActive;
        mix_object(p, Buildings.ID(p));
    }
    for (int i = 0; i < Aircraft.Count(); i++) {
        AircraftClass* p = Aircraft.Ptr(i);
        n[3] += p && p->IsActive;
        mix_object(p, Aircraft.ID(p));
    }
    for (int i = 0; i < Terrains.Count(); i++) {
        TerrainClass* p = Terrains.Ptr(i);
        n[4] += p && p->IsActive;
        mix_object(p, Terrains.ID(p));
    }
    for (int i = 0; i < Bullets.Count(); i++) {
        BulletClass* p = Bullets.Ptr(i);
        n[5] += p && p->IsActive;
        mix_object(p, Bullets.ID(p));
    }
    for (int i = 0; i < Anims.Count(); i++) {
        AnimClass* p = Anims.Ptr(i);
        n[6] += p && p->IsActive;
        if (p && p->IsActive) {
            mix((unsigned)p->Coord);
            mix_name(p->Class_Of().IniName);
        }
    }
    for (int i = 0; i < Houses.Count(); i++) {
        HouseClass* h = Houses.Ptr(i);
        if (h == NULL || !h->IsActive)
            continue;
        mix((unsigned)h->Class->House);
        mix((unsigned)h->Credits);
        mix((unsigned)h->Tiberium);
        mix(h->IsDefeated ? 1 : 0);
    }
    if (counts)
        memcpy(counts, n, sizeof(n));
    return se_h;
}
