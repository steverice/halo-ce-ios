/*
INPUT_REPLAY_SCENARIO.C

The game state input_replay.c needs and can't reach without the game's
headers: whether the main menu's scenario (ui.map) is loaded, since a
recording or replay starts with the first level, not the menu that loads
before it; and skipping a cinematic the way the accept button does
(player_control.c), for a replay that has reached the point where the
recording skipped it.
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/players.h"
#include "main/main.h"
#include "math/real_math.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"

unsigned char input_replay_main_menu_loaded(void)
{
	struct scenario *scenario = global_scenario_try_and_get();

	return scenario && scenario->type == _scenario_type_main_menu;
}

/* what an automatic take (debug.input_record = "auto") writes about the level it starts in: the map's
name without its folders, the difficulty (0 easy to 3 impossible), how many players this machine has, and the
game's random seed */
void input_replay_level(char *map, unsigned int size, long *difficulty, long *players, unsigned long *seed)
{
	const char *name = main_get_map_name();
	const char *cursor;
	unsigned int index = 0;

	for (cursor = name; cursor && *cursor; cursor++)
	{
		if (*cursor == '\\' || *cursor == '/')
			name = cursor + 1;
	}
	for (; name && name[index] && index + 1 < size; index++)
		map[index] = name[index];
	if (size)
		map[index] = 0;
	*difficulty = game_difficulty_level_get();
	*players = players_globals ? players_globals->local_player_count : 0;
	*seed = *get_global_random_seed_address();
}

void input_replay_skip_cinematic(void)
{
	if (cinematic_can_be_skipped())
		main_skip_cinematic();
}
