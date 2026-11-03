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
#undef protected
#undef private
#include <string.h>
#include "aoe_cores.h"

extern void aoe_view_join(void);
extern void aoe_view_whole(void);
extern void aoe_view_unhook(void);

// A menu is laid out when it is made (TEasy_Panel scales its places from 640 x 480 to the size then), and does not lay itself
// out again: a menu given the new size kept its old places on a larger page. So at a menu the change waits, through the menus
// after it, until the game screen comes (host.cpp's aoe_panel_changed posts it again then). The main menu made again at the
// new size (destroyed and made as the game goes back to it from another menu) took no more input in its one try: not done.
static int waiting;

extern "C" void aoe_layout_switch(int width)
{
    int height = width * 3 / 4;
    if (!rge_base_game || !rge_base_game->draw_system || !rge_base_game->draw_area) return;
    TPanel* panel = panel_system.currentPanel();
    const char* name = panel ? panel->panelName() : "";
    if (rge_base_game->draw_area->Width == width) { waiting = 0; return; }
    if (strcmp(name, "Game Screen")) {
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
}

extern "C" int aoe_layout_waiting(void) { return waiting; }
