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
#include "game/game_engine.h"
#include "game/players.h"
#include "game/player_queues_new.h"
#include "camera/observer.h"
#include "items/weapons.h"
#include "main/main.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "units/units.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"

#include <math.h>
#include <string.h>
#include "benchmark_mp_state.h"
#include "benchmark_mp_arena.h"
#include "network_test_suite.h"
#include "mp_benchmark_probe.h"

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

int network_test_suite_network_players(void);
int network_test_suite_map_matches(const char *map);

static void benchmark_mp_actor_fill(struct benchmark_mp_actor *actor, struct player_datum *player,
	const struct mp_benchmark_probe_counts *counts)
{
	struct unit_datum *unit;
	real_point3d origin;
	long weapon_index;

	actor->weapon_definition = NONE;
	if (!player)
		return;
	actor->present = 1;
	actor->repositions = network_test_suite_repositions(player->local_player_index != NONE ?
		player->local_player_index : player->network_player_data.machine_index);
	actor->kills = player->statistics.kills[0];
	actor->deaths = player->statistics.deaths;
	if (counts)
	{
		actor->fire_requests = counts->fire;
		actor->jump_requests = counts->jump;
		actor->grenade_requests = counts->grenade;
	}
	if (player->unit_index == NONE)
		return;
	actor->spawned = 1;
	object_get_origin(player->unit_index, &origin);
	actor->position[0] = origin.x;
	actor->position[1] = origin.y;
	actor->position[2] = origin.z;
	actor->body = object_get(player->unit_index)->object.body_vitality;
	actor->shield = object_get(player->unit_index)->object.shield_vitality;
	unit = unit_get(player->unit_index);
	actor->throttle = sqrtf(unit->unit.throttle.i * unit->unit.throttle.i +
		unit->unit.throttle.j * unit->unit.throttle.j);
	actor->grenades[0] = unit->unit.grenade_counts[0];
	actor->grenades[1] = unit->unit.grenade_counts[1];
	if (unit->unit.current_weapon_index == NONE)
		return;
	weapon_index = unit->unit.weapon_object_indices[unit->unit.current_weapon_index];
	if (weapon_index != NONE)
	{
		struct weapon_datum *weapon = weapon_get(weapon_index);

		actor->weapon_definition = weapon->definition_index;
		actor->ammo = weapon->weapon.magazines[0].rounds_total +
			weapon->weapon.magazines[0].rounds_loaded;
	}
}

void benchmark_scenario_multiplayer_state(struct benchmark_mp_state *state)
{
	struct scenario *scenario;
	struct game_variant *variant;
	short local;
	struct mp_benchmark_probe_counts counts[4];
	int network_players = network_test_suite_network_players();

	memset(state, 0, sizeof(*state));
	state->tick = -1;
	if (!game_time_initialized() || !game_in_progress())
		return;
	scenario = global_scenario_try_and_get();
	if (!scenario || scenario->type != _scenario_type_multiplayer ||
		game_connection() != _game_connection_network_server)
		return;
	state->multiplayer = 1;
	state->tick = game_time_get();
	state->map_ok = network_test_suite_map_matches(main_get_multiplayer_map_name()) ||
		(!network_players && mp_benchmark_probe_map_matches(main_get_multiplayer_map_name()));
	variant = game_engine_get_variant();
	state->slayer = variant && variant->game_engine_index == game_engine_slayer;
	state->score_limit = variant ? (int)variant->universal_variant.score_to_win : 0;
	state->time_limit_minutes = game_variant_options_get()->time_limit;
	state->local_players = players_port_local_player_count();
	state->view_count = main_get_window_count();
	state->can_score = game_engine_can_score();
	state->postgame = game_engine_showing_postgame();
	mp_benchmark_probe_action_counts(counts);
	/* Cardinality comes from every actual datum, not the bounded report slots. */
	{
		struct data_iterator iterator;
		struct player_datum *player;

		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			state->roster++;
			state->spawned += player->unit_index != NONE;
		}
	}
	for (local = 0; local < (network_players ? 1 : 4); local++)
	{
		long player_index = local_player_get_player_index(local);

		if (player_index != NONE)
			benchmark_mp_actor_fill(&state->actors[local], player_try_and_get(player_index), &counts[local]);
	}
	if (network_players >= 2 && network_players <= BENCHMARK_MP_ACTORS)
	{
		short action_count = 0;
		long update_number = update_server_ticked_update_number();
		struct player_action const *actions = update_number != NONE ?
			update_server_update_actions(update_number, &action_count) : NULL;
		struct object_iterator iterator;

		state->synthetic_queue_ready = benchmark_mp_arena_ready();
		for (int i = 1; i < network_players; i++)
		{
			long *players = machine_get_player_list(i);
			long player_index = players ? players[0] : NONE;
			struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
			short absolute_index = player_index != NONE ? (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) : -1;

			if (!player || player->local_player_index != NONE || player->network_player_data.machine_index != i)
			{
				state->synthetic_queue_ready = 0;
				continue;
			}
			benchmark_mp_actor_fill(&state->actors[i], player, NULL);
			if (!actions || absolute_index < 0 || absolute_index >= action_count ||
				(actions[absolute_index].throttle.i == 0.0f && actions[absolute_index].throttle.j == 0.0f &&
				actions[absolute_index].primary_trigger == 0.0f))
				state->synthetic_queue_ready = 0;
		}
		object_iterator_new(&iterator, _object_mask_projectile, 0);
		while (object_iterator_next(&iterator) != NULL)
			state->projectiles_alive++;
	}
	network_test_suite_scripted_counts(&state->scripted_hits, &state->scripted_kills);
	state->observer0 = state->actors[0].present && observer_get_camera(0) != NULL;
	state->probe_active = mp_benchmark_probe_active();
}
