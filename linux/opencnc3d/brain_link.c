// SPDX-License-Identifier: GPL-3.0-or-later
// ShaderEmu: OpenCNC3D's renderer loads its brain with dlopen and finds its CNC_* and OpenCNC3D_*
// functions with dlsym (cnc_eyes.cpp, load_brain). The machine loads no library: the brain is linked
// into the program, and these four answer for it from a table, whatever path is asked for.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRAIN(X) \
    X(CNC_Add_Mod_Path) X(CNC_Advance_Instance) X(CNC_Clear_Object_Selection) X(CNC_Config) X(CNC_Get_Game_State) \
    X(CNC_Handle_ControlGroup_Request) X(CNC_Handle_Debug_Request) X(CNC_Handle_Input) X(CNC_Handle_Sidebar_Request) \
    X(CNC_Handle_Structure_Request) X(CNC_Handle_SuperWeapon_Request) X(CNC_Handle_Unit_Request) X(CNC_Init) \
    X(CNC_Save_Load) X(CNC_Select_Object) X(CNC_Set_Multiplayer_Data) X(CNC_Start_Custom_Instance) \
    X(CNC_Start_Instance_Variation) X(OpenCNC3D_ABI_Facts) X(OpenCNC3D_AI_Diagnostics) X(OpenCNC3D_AI_Policy) \
    X(OpenCNC3D_AI_State_Hash) X(OpenCNC3D_Cell_To_Pixel) X(OpenCNC3D_Drain_Events) X(OpenCNC3D_Dump_Objects) \
    X(OpenCNC3D_Dump_Selection) X(OpenCNC3D_Event_ABI) X(OpenCNC3D_Force_Fire) X(OpenCNC3D_Force_Verdict) \
    X(OpenCNC3D_Get_Build_Potential) X(OpenCNC3D_Get_Rally) X(OpenCNC3D_Get_View) X(OpenCNC3D_Grant_Superweapons) \
    X(OpenCNC3D_Hurt_Object) X(OpenCNC3D_Player_Blowup) X(OpenCNC3D_Player_Is_AI) X(OpenCNC3D_Player_To_AI) \
    X(OpenCNC3D_Post_Event) X(OpenCNC3D_Probe_Object_At) X(OpenCNC3D_Proximity_Ok) X(OpenCNC3D_Sel_Info) \
    X(OpenCNC3D_Set_AI_Difficulties) X(OpenCNC3D_Set_Build_Anywhere) X(OpenCNC3D_Set_Campaign_Difficulty) \
    X(OpenCNC3D_Set_Computer_Difficulty) X(OpenCNC3D_Set_Fast_Shells) X(OpenCNC3D_Set_Free_Infantry) \
    X(OpenCNC3D_Set_Invincible) X(OpenCNC3D_Set_Lockstep) X(OpenCNC3D_Set_Rally) X(OpenCNC3D_Set_Seed) \
    X(OpenCNC3D_Set_Wall_Fill) X(OpenCNC3D_Spawn_Anim) X(OpenCNC3D_Spring_Trigger) X(OpenCNC3D_Store_Ore) \
    X(OpenCNC3D_Trigger_Info)

// weak: what only the Red Alert brain has (OpenCNC3D_Set_Seed, _ABI_Facts, ...) is 0, as its dlsym would be
#define DECLARE(n) extern void n(void) __attribute__((weak));
BRAIN(DECLARE)
#define ENTRY(n) {#n, (void*)n},
static const struct { const char* name; void* fn; } brain[] = { BRAIN(ENTRY) };

static char error[128];
// what Half-Life's GL shim (gl_ocnc.c) reads of Half-Life's server in a debugging print: never written here
struct se_asks_t { unsigned fn[64], n, unused[3]; } se_asks;
static int the_brain;   // the handle: any address that is not 0

void* dlopen(const char* path, int flags) {
    (void)flags;
    if (getenv("OPENCNC3D_GL_LOG")) fprintf(stderr, "ocnc: the brain asked for at %s is the one linked in\n", path ? path : "(null)");
    return &the_brain;
}
void* dlsym(void* handle, const char* name) {
    if (handle != &the_brain) { snprintf(error, sizeof error, "no such library"); return 0; }
    for (unsigned i = 0; i < sizeof brain / sizeof brain[0]; i++)
        if (!strcmp(brain[i].name, name) && brain[i].fn) return brain[i].fn;
    snprintf(error, sizeof error, "%s is not in the brain linked in", name);
    return 0;
}
// The 1995 view (DLLExportClass::Legacy_Render_Enabled, brain_shaderemu.cpp): CNC_Advance_Instance drew the DOS game's
// view into its hidden page at every tick, 3.69 million instructions of a 3.77 million tick in play, and OpenCNC3D's eyes
// never read that page. Off, as in opencnc3d-brain, unless OPENCNC3D_RENDER=1; the game's sums are the same either way
// (docs/games/opencnc3d.md, "The 1995 view").
void ShaderEmu_Set_Legacy_Render(int on);
__attribute__((constructor)) static void brain_link_render(void) {
    const char* render = getenv("OPENCNC3D_RENDER");
    ShaderEmu_Set_Legacy_Render(render && atoi(render));
}
int dlclose(void* handle) { (void)handle; return 0; }
char* dlerror(void) { return error[0] ? error : 0; }
