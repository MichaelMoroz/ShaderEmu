// SPDX-License-Identifier: GPL-3.0-or-later
// ShaderEmu: the object dump between OpenCNC3D's brain and its renderer as numbers (docs/games/opencnc3d.md,
// "The object dump"). OpenCNC3D_Dump_Objects printed an OBJ| line an object at every tick, which the renderer
// split and read back by key; while the renderer captures with records (ShaderEmu_Dump_On 2), the brain fills one
// of these an object instead, with what the line's fields were, in the line's order, and the renderer reads it
// through the same keys. The other lines of the dump stay text. Included by our patch's dllinterface.cpp (build.sh
// copies it beside brain_shaderemu.cpp) and cnc_eyes.cpp.
#ifndef SE_DUMP_H
#define SE_DUMP_H

// the line's numbers by name, in its order: OBJ|kind|type|owner|newcell=..|x=..|...|act=..
#define SE_OBJ_FIELDS(X) \
    X(newcell) X(x) X(y) X(inicell) X(centercell) X(str) X(max) X(limbo) X(down) \
    X(lx) X(ly) X(clx) X(cly) X(fw) X(fh) X(face) X(tface) X(id) \
    X(sel) X(nav) X(tar) X(doing) X(dostage) X(dying) X(makecnt) X(dimw) X(door) X(status) \
    X(blush) X(flash) X(tcx) X(tcy) X(pips) X(maxpips) X(alt) \
    X(cloak) X(cstage) X(repairing) X(wrench) X(poweroff) \
    X(harv) X(rotating) X(navset) X(driving) X(primary) X(group) X(flashme) X(act)

#define SE_OBJ_ENUM(n) SE_OBJ_##n,
enum { SE_OBJ_FIELDS(SE_OBJ_ENUM) SE_OBJ_COUNT };
#undef SE_OBJ_ENUM

typedef struct ShaderEmu_ObjRec {
    const char* kind;       // INFANTRY, UNIT, BUILDING, TERRAIN, AIRCRAFT
    const char* type;       // the type's IniName
    const char* owner;      // the house's IniName, or None
    const char* mission;    // the mission's name, or -
    int v[SE_OBJ_COUNT];
} ShaderEmu_ObjRec;

#ifdef __cplusplus
extern "C" {
#endif
// the dump's capture: 0 none (printf as it was), 1 the lines into ShaderEmu_Dump_Buf, 2 those but the objects as
// records, 3 both the records and their lines (OPENCNC3D_DUMP=check: the renderer compares them)
extern int ShaderEmu_Dump_On;
extern char* ShaderEmu_Dump_Buf;
extern unsigned ShaderEmu_Dump_Len;
extern ShaderEmu_ObjRec* ShaderEmu_Obj_Recs;
extern unsigned ShaderEmu_Obj_Count;
extern const char* const ShaderEmu_Obj_Names[SE_OBJ_COUNT];
ShaderEmu_ObjRec* ShaderEmu_Obj_Next(void);
#ifdef __cplusplus
}
#endif
#endif
