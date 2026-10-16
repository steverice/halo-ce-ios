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
#include "ai/actors.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/players.h"
#include "main/main.h"
#include "math/real_math.h"
#include "objects/objects.h"
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

unsigned long input_replay_random_seed(const unsigned long *seed)
{
	if (seed)
		*get_global_random_seed_address() = *seed;
	return *get_global_random_seed_address();
}

/* both of the game's random sequences (render_random.c says which is which): the global one, which the
simulation draws from, and the local one, which a tick also draws from; read into seeds, or set from them */
void input_replay_random_seeds(unsigned long seeds[2], unsigned char set)
{
	if (set)
	{
		*get_global_random_seed_address() = seeds[0];
		*get_global_local_random_seed_address() = seeds[1];
	}
	else
	{
		seeds[0] = *get_global_random_seed_address();
		seeds[1] = *get_global_local_random_seed_address();
	}
}

/* player 1's position, for debug.input_replay_route; 0 while there is no player unit */
unsigned char input_replay_player_position(float position[3])
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

/* the fight's state, for debug.input_replay_route: whether player 1 is alive, the unit's body and shield, how many
actors the AI has and how many objects are dead; 0 while there is no player unit */
unsigned char input_replay_fight_state(float *body, float *shield, long *actors, long *dead_objects)
{
	long player_index = local_player_get_player_index(0);
	struct player_datum *player;
	struct object_datum *unit;
	struct data_iterator iterator;
	struct object_header_datum *header;

	if (player_index == NONE)
		return 0;
	player = player_try_and_get(player_index);
	if (!player || player->unit_index == NONE)
		return 0;
	unit = object_try_and_get(player->unit_index);
	if (!unit)
		return 0;
	*body = unit->object.body_vitality;
	*shield = unit->object.shield_vitality;
	*actors = 0;
	data_iterator_new(&iterator, actor_data);
	while (data_iterator_next(&iterator))
		(*actors)++;
	*dead_objects = 0;
	data_iterator_new(&iterator, object_header_data);
	while ((header = data_iterator_next(&iterator)))
		if (header->datum && (header->datum->object.damage_flags & (1 << _object_dead_bit)))
			(*dead_objects)++;
	return 1;
}

/* 1 if there was a cinematic to skip and it was asked to end */
unsigned char input_replay_skip_cinematic(void)
{
	if (!cinematic_can_be_skipped())
		return 0;
	main_skip_cinematic();
	return 1;
}
