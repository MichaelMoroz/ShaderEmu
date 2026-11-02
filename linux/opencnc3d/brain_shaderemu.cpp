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

// what Half-Life's GL shim (eyes.c) reads of Half-Life's server in a debugging print: never written here
extern "C" {
struct se_asks_t { unsigned fn[64], n, unused[3]; } se_asks;
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

// The objects to draw, for linux/opencnc3d/eyes.c: what OpenCNC3D_Dump_Objects prints of each live
// object outside limbo, as numbers. kind: 0 infantry, 1 unit, 2 building, 3 terrain, 4 aircraft.
struct ShaderEmu_Object {
    int kind;
    char type[12];
    int house;      // HousesType: 0 GDI (GoodGuy), 1 Nod (BadGuy), ...
    int x, y;       // the centre, in leptons (256 a cell)
    int face, turret; // 0..255 clockwise from north; turret -1 when none
    int strength, max_strength;
};
static int se_put(ShaderEmu_Object* out, int n, int max, ObjectClass* o, int kind)
{
    if (o == NULL || !o->IsActive || o->IsInLimbo || n >= max)
        return n;
    ShaderEmu_Object& s = out[n];
    s.kind = kind;
    strncpy(s.type, o->Class_Of().IniName, sizeof(s.type) - 1);
    s.type[sizeof(s.type) - 1] = 0;
    s.house = (int)o->Owner();
    COORDINATE c = o->Center_Coord();
    s.x = Coord_X(c);
    s.y = Coord_Y(c);
    s.face = -1;
    s.turret = -1;
    if (o->Is_Techno()) {
        TechnoClass* t = (TechnoClass*)o;
        s.face = (int)t->PrimaryFacing.Current();
        if (kind == 1 && ((UnitClass*)o)->Class->IsTurretEquipped)
            s.turret = (int)((UnitClass*)o)->SecondaryFacing.Current();
    }
    s.strength = o->Strength;
    s.max_strength = o->Class_Of().MaxStrength;
    return n + 1;
}
extern "C" int ShaderEmu_Objects(ShaderEmu_Object* out, int max)
{
    int n = 0;
    for (int i = 0; i < Terrains.Count(); i++)
        n = se_put(out, n, max, Terrains.Ptr(i), 3);
    for (int i = 0; i < Buildings.Count(); i++)
        n = se_put(out, n, max, Buildings.Ptr(i), 2);
    for (int i = 0; i < Units.Count(); i++)
        n = se_put(out, n, max, Units.Ptr(i), 1);
    for (int i = 0; i < Aircraft.Count(); i++)
        n = se_put(out, n, max, Aircraft.Ptr(i), 4);
    for (int i = 0; i < Infantry.Count(); i++)
        n = se_put(out, n, max, Infantry.Ptr(i), 0);
    return n;
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
