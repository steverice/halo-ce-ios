/*
BENCHMARK_SCENARIO.C

What the device benchmark's suite (port/linux/src/benchmark_suite.c) needs
from the game and can't reach without the game's headers: starting a level at
a difficulty or going back to the main menu, as the console's map_name and
the pause menu do; where the game is; and where player 1 stands, for the
check that a replay stayed on its route.
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/players.h"
#include "main/main.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"

#include <string.h>

/* starts map (the console's map_name) at a difficulty by the console's name
for it; an empty map is the main menu */
void benchmark_scenario_load(const char *map, const char *difficulty)
{
	static const char *const names[] = { "easy", "normal", "hard", "impossible" };
	short level;

	if (!map || !*map)
	{
		main_goto_main_menu();
		return;
	}
	/* (the next map's difficulty, as the console's game_difficulty_set sets it: game_difficulty_level_set changes only
	the game in progress, and the map load overwrites it with this one) */
	for (level = 0; level < 4; level++)
		if (!strcmp(difficulty, names[level]))
			main_set_difficulty(level);
	main_set_map_name(map);
}

/* 1 while the main menu's scenario is the one loaded */
unsigned char benchmark_scenario_at_menu(void)
{
	struct scenario *scenario = global_scenario_try_and_get();

	return scenario && scenario->type == _scenario_type_main_menu;
}

unsigned char benchmark_scenario_in_cinematic(void)
{
	return cinematic_in_progress() ? 1 : 0;
}

/* player 1's position in the world; 0 while there is no player or no body */
unsigned char benchmark_scenario_player_position(float *position)
{
	long player_index = local_player_get_player_index(0);
	struct player_datum *player;
	real_point3d origin;

	if (player_index == NONE)
		return 0;
	player = player_try_and_get(player_index);
	if (!player || player->unit_index == NONE)
		return 0;
	object_get_origin(player->unit_index, &origin);
	position[0] = origin.x;
	position[1] = origin.y;
	position[2] = origin.z;
	return 1;
}
