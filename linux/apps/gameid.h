/*
 * The program whose window has the keyboard, as a number the host can read: the desktop's bar
 * writes it once a second to the control word at 0x81000068, and the world's game controller
 * picks its controls by it (unity/ShaderEmu/Scripts/EmuGamepad.cs has the same numbers).
 */
#ifndef GAMEID_H
#define GAMEID_H

#include <string.h>

#define GAME_WORD 26	/* of the control words: 0x68 / 4 */

enum { GAME_NONE, GAME_DOOM, GAME_QUAKE, GAME_CLASSICUBE, GAME_TDAWN, GAME_RALERT, GAME_NES, GAME_QUAKE3, GAME_PHYS, GAME_HALFLIFE };

/* By the name the kernel has for the process (/proc/PID/comm). */
static int
game_of(const char *comm)
{
	static const struct { const char *name; int id; } games[] = {
		{ "doom", GAME_DOOM }, { "quake", GAME_QUAKE }, { "classicube", GAME_CLASSICUBE },
		{ "tdawn", GAME_TDAWN }, { "ralert", GAME_RALERT }, { "nes", GAME_NES }, { "quake3", GAME_QUAKE3 },
		{ "nxphys", GAME_PHYS },	/* both hands take boxes: docs/apps/physics.md, "The controls" */
		{ "halflife", GAME_HALFLIFE },
	};
	unsigned i;

	for (i = 0; i < sizeof games / sizeof games[0]; i++) {
		size_t n = strlen(games[i].name);

		if (!strncmp(comm, games[i].name, n) && (comm[n] == 0 || comm[n] == '.'))
			return games[i].id;
	}
	return GAME_NONE;
}

#endif
