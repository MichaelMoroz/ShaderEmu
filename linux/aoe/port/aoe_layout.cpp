// The game's layout made again for the window's size (docs/games/aoe.md, "Two layouts"): what the game's own options did
// when a player chose another screen size (RGE_Base_Game::set_screen_size: the surfaces at the new size, a WM_SIZE that lays
// out the panel up, and the game screen's pictures taken again for that width). Called where a message is handled
// (win32.cpp, AOE_WM_LAYOUT), with the world's update and the view's job done first.
#include <stdio.h>
#include <stdlib.h>
#define protected public
#define private public
#include "../include/basegame.h"
#include "../include/drawarea.h"
#include "../include/panel.h"
#include "../include/game_globals.h"
#include "../include/scr_main.h"
#include "../include/scr_sing.h"
#include "../include/scr_mp.h"
#undef protected
#undef private
#include <string.h>
#include "aoe_cores.h"
#include "aoe_window.h"
#undef fopen   // (win/io.h's: the game's own files by their Windows names; /tmp/aoe-layout is the machine's)

extern void aoe_post(unsigned m, unsigned a, long b);   // (win32.cpp; aoe_port.h has it, with windows.h's types)
extern void aoe_view_join(void);
extern void aoe_view_whole(void);
extern void aoe_view_unhook(void);

// A menu is laid out when it is made (TEasy_Panel scales its places from 640 x 480 to the size then) and does not lay itself out
// again: given the new size it kept its old places on a larger page. So a menu that holds nothing chosen is made again at the new
// size, as the game makes it when it goes there from another menu (scr_main.cpp, scr_sing.cpp): the main menu, the single player
// menu and the multiplayer start. Another menu (one with settings chosen in it, a list, a scenario) keeps its layout, and the change
// waits, through the menus after it, until one of those or the game screen comes (host.cpp's aoe_panel_changed posts it again).
// Making a menu at 800 x 600 draws its 800-pixel picture three times (setCurrentPanel's focus, redraw and UpdateWindow), about
// 41 million instructions each: on rvc_cpu's clock half a minute of the guest's, which was taken for a stop before 4 November 2026.
static int waiting;

static const char* const remade[] = { "Main Menu", "Single Player Menu", "MP Startup Screen" };

static int remakes(const char* name)
{
    for (unsigned i = 0; i < sizeof remade / sizeof remade[0]; i++)
        if (!strcmp(name, remade[i])) return 1;
    return 0;
}

// The menu up made again at the page's size: the new one made and shown, then the old one (renamed meanwhile) destroyed.
static void remake_menu(void)
{
    TPanel* panel = panel_system.currentPanel();
    if (!panel || !remakes(panel->panelName())) return;
    char name[64];
    snprintf(name, sizeof name, "%s", panel->panelName());
    static char old_name[] = "Menu laid out before";
    free(panel->panel_name);
    panel->panel_name = strdup(old_name);
    rge_base_game->disable_input();
    if (!strcmp(name, "Main Menu")) new TRIBE_Screen_Main_Menu;
    else if (!strcmp(name, "Single Player Menu")) new TribeSPMenuScreen;
    else new TribeMPStartupScreen;
    panel_system.setCurrentPanel(name, 0);
    panel_system.destroyPanel(old_name);
    fprintf(stderr, "aoe: %s made again at %ld x %ld\n", name, (long) rge_base_game->draw_area->Width, (long) rge_base_game->draw_area->Height);
    // (for a test: the layout a menu now has, after the script's "ready")
    FILE* f = fopen("/tmp/aoe-layout.part", "w");
    if (f) { fprintf(f, "%ld\n", (long) rge_base_game->draw_area->Width); fclose(f); rename("/tmp/aoe-layout.part", "/tmp/aoe-layout"); }
}

// The height a layout WIDTH wide has at the panel NAME: the game screen the window's shape, anything else 4:3.
static int height_at(const char* name, int width)
{
    return !strcmp(name, "Game Screen") ? aoe_win_game_height(width) : width * 3 / 4;
}

extern "C" int aoe_layout_height_wrong(const char* name)
{
    if (!rge_base_game || !rge_base_game->draw_area) return 0;
    long w = rge_base_game->draw_area->Width;
    return w > 0 && rge_base_game->draw_area->Height != height_at(name, (int) w);
}

extern "C" void aoe_layout_switch(int width)
{
    if (!rge_base_game || !rge_base_game->draw_system || !rge_base_game->draw_area) return;
    if (width == 1) { remake_menu(); return; }   // (posted below: done at the next message)
    TPanel* panel = panel_system.currentPanel();
    const char* name = panel ? panel->panelName() : "";
    int height = height_at(name, width);
    if (rge_base_game->draw_area->Width == width && rge_base_game->draw_area->Height == height) { waiting = 0; return; }
    int menu = remakes(name);
    if (strcmp(name, "Game Screen") && !menu) {
        if (!waiting) fprintf(stderr, "aoe: layout %d x %d waits for the next panel (at %s)\n", width, height, name);
        waiting = width;
        return;
    }
    waiting = 0;
    aoe_world_finish();
    aoe_view_join();
    fprintf(stderr, "aoe: layout %d x %d at %s (from %ld x %ld)\n", width, height, name, (long) rge_base_game->draw_area->Width,
            (long) rge_base_game->draw_area->Height);
    aoe_view_unhook();
    rge_base_game->set_screen_size(width, height);
    aoe_view_whole();
    if (menu)
        aoe_post(AOE_WM_LAYOUT, 1, 0);
}

extern "C" int aoe_layout_waiting(void) { return waiting; }
extern "C" int aoe_layout_remakes(const char* name) { return remakes(name); }
