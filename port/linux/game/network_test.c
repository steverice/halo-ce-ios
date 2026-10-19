/*
NETWORK_TEST.C

Automated system link sessions for testing the netcode without the menus
(debug.network_test in config.toml, HALO_NETWORK_TEST):

- "host:<map>[:<variant>[,<variant>...]]" hosts a game on that
  multiplayer map (bloodgulch, ...) with one of the built-in game variants
  (slayer by default; game_engine_get_variant_by_name), as the pregame
  screen's fast setup does, and starts it debug.network_test_start seconds
  later; with more variants, once a game is over (debug.network_test_score
  makes it short) the next, as the host's button on the scores does. A map
  with a path is that level (custom_maps\a30, a Custom Edition map's), and
  the variant "coop" makes the game network co-op on it, at normal
  difficulty, as the Map screen does for a campaign level;
- "join" searches for games and joins the first it finds, as picking it in
  the system link list does;
- "browse" joins the first game the server browser lists (internet play's
  public games: a host's with debug.network_test_public), as picking it
  there does, and then as "join".

Once the game runs, every second each machine logs where every player's
unit is, so the machines' views of the game can be compared.

Scripted play for the netcode's parts the bots' wandering does not reach:
debug.network_test_kill (the host kills the last player every so often),
debug.network_test_shoot (every so often each machine's player hits the
next with their weapon's projectile: a client's through its report to the
host) and debug.network_test_vehicle (the host seats the last player as a
vehicle's driver that many seconds in, takes them out 15 seconds on, and 5
seconds later stands them at a vehicle's driver's entrance, where a joining
machine's player holds the action button, as getting in does)
and debug.network_test_pickup (the last player stands on a weapon lying
about that many seconds in, and a joining machine's player holds the action
button a second later, to pick it up).

Called from the main loop every frame (main.c).
*/

#include "cseries.h"
#include "main/main.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "networking/network_server_message_handler.h"
#include "networking/network_game_manager.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "game/player_queues_new.h"
#include "objects/objects.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "items/projectile_definitions.h"
#include "items/items.h"
#include "objects/damage.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "camera/observer.h"
#include "network_test_suite.h"
#include "benchmark_mp_arena.h"

#include "../src/p2p.h" /* port: port/linux/src/p2p.c, p2p_lobby.c */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
int config_boolean(char const *name);
double config_real(char const *name);
long config_integer(char const *name);
void platform_log(char const *format, ...);
/* damage.c's */
void damage_kill_object_for_player(long object_index, long player_index);
/* network_distributed.c's */
void network_distributed_statistics(long *sent, long *received, long *corrections);
void network_distributed_item_statistics(long *creates, long *deletes, long *failures, long *removed);
void network_damage_statistics(long *sent_reports, long *dealt_reports, long *rejected_reports, long *replayed_events);
/* xinput_sdl.c's */
void test_input_hold_action(int hold);

enum
{
	_network_test_off,
	_network_test_host,
	_network_test_join,
	_network_test_browse,
};

struct network_test_weapon_trace
{
	int ready;
	long unit_index, weapon_index, definition_index;
	short slot;
	unsigned long generation, reported_slots;
	uint32_t projectiles;
};

static struct
{
	boolean checked;
	short mode;
	char map_name[64];
	char variant_name[64];
	/* ... the variant of this game, of variant_name's list; and the seconds
	the last game's scores have been shown */
	short variant_index;
	real postgame_seconds;
	boolean game_over;
	real start_delay;
	real menu_seconds;
	boolean set_up;
	real setup_seconds;
	boolean started;
	boolean joined;
	boolean map_set;
	boolean player_added;
	short players_added;
	short players_wanted;
	real joined_seconds;
	boolean team_set;
	real kill_interval;
	real shoot_interval;
	real vehicle_time;
	real pickup_time;
	char pickup_weapon[64];
	long score_to_win;
	short suite_time_limit_minutes;
	short suite_network_players;
	unsigned long suite_repositions[BENCHMARK_MP_MAXIMUM_PLAYERS], suite_scripted_hits, suite_scripted_kills;
	long logged_time;
	boolean public_game;
	boolean browsing;
	boolean suite_owned;
	boolean suite_synthetic_added;
	unsigned long suite_setup_input_ticks;
	boolean suite_setup_input_reported;
	uint32_t suite_energy_projectiles[BENCHMARK_MP_MAXIMUM_PLAYERS];
	long suite_weapon_units[BENCHMARK_MP_MAXIMUM_PLAYERS];
	int suite_weapon_ready[BENCHMARK_MP_MAXIMUM_PLAYERS];
	long suite_weapon_definitions[6];
	int suite_weapon_definitions_ready;
	struct network_test_weapon_trace suite_weapon_trace[BENCHMARK_MP_MAXIMUM_PLAYERS];
} network_test;

int network_test_suite_begin(const char *simulation, const char *map, const char *game_mode,
	int players, int views, int score_limit, int time_limit_minutes)
{
	int local_players;

	size_t map_length, i;

	if (!simulation || !map || !game_mode || strcmp(game_mode, "slayer") ||
		views != 1 || score_limit < 1 || score_limit > 1000 || time_limit_minutes < 0 || time_limit_minutes > 120)
		return 0;
	map_length = strlen(map);
	if (!map_length || map_length >= sizeof(network_test.map_name))
		return 0;
	for (i = 0; i < map_length; i++)
		if (!((map[i] >= 'a' && map[i] <= 'z') || (map[i] >= '0' && map[i] <= '9') || map[i] == '_'))
			return 0;
	if (!strcmp(simulation, "local_controllers") && players == 4)
		local_players = 4;
	else if (!strcmp(simulation, "network_players") && players >= 2 && players <= BENCHMARK_MP_MAXIMUM_PLAYERS)
		local_players = 1;
	else
		return 0;
	if (network_test.suite_owned)
		return network_test.mode == _network_test_host && network_test.players_wanted == local_players &&
			network_test.score_to_win == score_limit && network_test.suite_time_limit_minutes == time_limit_minutes &&
			network_test.suite_network_players == (local_players == 1 ? players : 0) && !strcmp(network_test.map_name, map);
	if (*config_string("debug.network_test") || network_test.mode != _network_test_off ||
		game_connection() != _game_connection_local)
		return 0;
	memset(&network_test, 0, sizeof(network_test));
	network_test.checked = TRUE;
	network_test.suite_owned = TRUE;
	network_test.mode = _network_test_host;
	snprintf(network_test.map_name, sizeof(network_test.map_name), "%s", map);
	snprintf(network_test.variant_name, sizeof(network_test.variant_name), "%s", game_mode);
	network_test.players_wanted = (short)local_players;
	network_test.suite_network_players = local_players == 1 ? (short)players : 0;
	network_test.start_delay = 15.0f;
	network_test.score_to_win = score_limit;
	network_test.suite_time_limit_minutes = (short)time_limit_minutes;
	network_test.shoot_interval = 5.0f;
	network_test.kill_interval = 25.0f;
	return 1;
}

int network_test_suite_network_players(void)
{
	return network_test.suite_owned && network_test.mode == _network_test_host ? network_test.suite_network_players : 0;
}

int network_test_suite_map_matches(const char *map)
{
	const char *base = map, *p;

	if (!network_test.suite_owned || network_test.mode != _network_test_host || !map)
		return 0;
	for (p = map; *p; p++)
		if (*p == '\\' || *p == '/')
			base = p + 1;
	return !strcmp(base, network_test.map_name);
}

unsigned long network_test_suite_repositions(int actor)
{
	return network_test.suite_owned && actor >= 0 && actor < BENCHMARK_MP_MAXIMUM_PLAYERS ?
		network_test.suite_repositions[actor] + benchmark_mp_arena_repositions(actor) : 0;
}

uint32_t network_test_suite_energy_projectiles(int actor)
{
	return network_test.suite_owned && actor >= 0 && actor < BENCHMARK_MP_MAXIMUM_PLAYERS && actor < network_test.suite_network_players ?
		network_test.suite_energy_projectiles[actor] : 0;
}

static int network_test_synthetic_machine_name(wchar_t *name, size_t capacity, int machine_index)
{
	int serial = machine_index + 1;

	if (!name || capacity < 5 || machine_index < 1 || machine_index >= BENCHMARK_MP_MAXIMUM_PLAYERS)
		return 0;
	name[0] = L'B';
	name[1] = L'0' + serial / 100;
	name[2] = L'0' + (serial / 10) % 10;
	name[3] = L'0' + serial % 10;
	name[4] = 0;
	return 1;
}

void network_test_suite_projectile_created(long owner_unit, int energy)
{
	long player_index, *players;
	struct player_datum *player;
	int actor, i;

	if (!energy || !network_test.suite_owned || network_test.mode != _network_test_host ||
		network_test.suite_network_players < 2 || network_test.suite_network_players > BENCHMARK_MP_MAXIMUM_PLAYERS || owner_unit == NONE ||
		game_connection() != _game_connection_network_server)
		return;
	player_index = player_index_from_unit_index(owner_unit);
	player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	if (!player || player->unit_index != owner_unit)
		return;
	actor = player->network_player_data.machine_index;
	if (actor < 0 || actor >= network_test.suite_network_players || player->local_player_index != (actor ? NONE : 0))
		return;
	players = machine_get_player_list(actor);
	if (!players || players[0] != player_index)
		return;
	for (i = 1; i < 4; i++)
		if (players[i] != NONE) return;
	if (network_test.suite_energy_projectiles[actor] != UINT32_MAX)
		network_test.suite_energy_projectiles[actor]++;
}

void network_test_suite_scripted_counts(unsigned long *hits, unsigned long *kills)
{
	*hits = network_test.suite_owned ? network_test.suite_scripted_hits : 0;
	*kills = network_test.suite_owned ? network_test.suite_scripted_kills : 0;
}

int network_test_suite_failed(void)
{
	return network_test.suite_owned && network_test.mode == _network_test_off;
}

void network_test_suite_end(void)
{
	int abort_game;

	if (!network_test.suite_owned)
		return;
	abort_game = game_connection() == _game_connection_network_server;
	benchmark_mp_arena_reset();
	memset(&network_test, 0, sizeof(network_test));
	network_test.checked = TRUE;
	if (abort_game)
		network_game_abort();
}

/* Synthetic participants occupy real game roster slots, but do not create
transport clients or local controllers. Insert the complete roster atomically. */
static int network_test_suite_add_synthetic_players(struct network_game_server *server)
{
	struct network_game *game = network_game_get_game();
	int added = 0, target = network_test.suite_network_players, i;

	if (!server || !game || target < 2 || target > BENCHMARK_MP_MAXIMUM_PLAYERS || game->machine_count != 1 || game->player_count != 1 ||
		game->machines[0].machine_index != 0 || game->maximum_players < target)
		return 0;
	for (i = 1; i < target; i++)
		if (game->machines[i].machine_index != NONE)
			return 0;
	for (i = 0; i < (int)(sizeof(game->players) / sizeof(game->players[0])); i++)
		if (game->players[i].machine_index != NONE &&
			(game->players[i].machine_index != 0 || game->players[i].controller_index != 0))
			return 0;
	for (i = 1; i < target; i++)
	{
		struct network_machine machine = { 0 };
		struct network_player player = { 0 };

		machine.machine_index = (char)i;
		if (!network_test_synthetic_machine_name(machine.name,
			sizeof(machine.name) / sizeof(machine.name[0]), i))
			goto rollback;
		player.machine_index = (char)i;
		player.controller_index = 0;
		player.team_index = NONE;
		player.player_list_index = NONE;
		memcpy(player.name, machine.name, 5 * sizeof(player.name[0]));
		if (!network_game_add_machine(game, &machine))
			goto rollback;
		added++;
		if (!network_game_add_player(game, &player))
			goto rollback;
	}
	if (network_game_server_send_game_data_pregame(server))
		return 1;
rollback:
	for (i = added; i > 0; i--)
		network_game_remove_machine(game, &game->machines[i]);
	return 0;
}

/* A sent local-player request is asynchronous; only the server's actual roster confirms it. */
static int network_test_suite_prepare_synthetic_players(struct network_game_server *server)
{
	struct network_game *game;

	if (network_test.suite_synthetic_added)
		return 1;
	if (!network_test.player_added || !server)
		return 0;
	game = network_game_server_get_game(server);
	if (!game || game->player_count == 0)
		return 0;
	if (!network_test_suite_add_synthetic_players(server))
		return -1;
	network_test.suite_synthetic_added = TRUE;
	platform_log("network test: synthetic roster ready after local player registration");
	return 1;
}


/* Unlike unit_delete_all_weapons, remove the current weapon too. Immediate
object deletion notifies the unit before replacement needs its inventory slots. */
static void network_test_suite_clear_bot_weapons(long unit_index)
{
	short slot;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		struct unit_datum *unit = unit_try_and_get(unit_index);
		long weapon_index = unit ? unit->unit.weapon_object_indices[slot] : NONE;

		if (weapon_index != NONE) object_delete_immediately(weapon_index);
	}
}


/* Observe the readied object, including changes that finish between input ticks.
A respawn establishes a new baseline rather than pretending it was a weapon swap. */
static void network_test_suite_observe_weapon(int actor, long unit_index)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);
	struct network_test_weapon_trace *trace = &network_test.suite_weapon_trace[actor];
	struct weapon_datum *weapon;
	long weapon_index;
	short slot;

	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit)) return;
	slot = unit->unit.current_weapon_index;
	if (slot < 0 || slot >= MAXIMUM_WEAPONS_PER_UNIT) return;
	weapon_index = unit->unit.weapon_object_indices[slot];
	weapon = weapon_try_and_get(weapon_index);
	if (!weapon || !TEST_FLAG(weapon->item.flags, _item_attached_to_unit_bit) ||
		weapon->object.owner_player_index != unit->unit.player_index) return;
	if (!trace->ready || trace->unit_index != unit_index)
	{
		memset(trace, 0, sizeof(*trace));
		trace->ready = 1;
		trace->unit_index = unit_index;
	}
	else if (trace->weapon_index != weapon_index)
	{
		if (trace->slot != slot && trace->definition_index != weapon->definition_index)
		{
			trace->generation++;
			platform_log("network test: bot actual switch actor %d tick %ld unit %lx generation %lu from_slot %d from_weapon %lx from_definition %lx to_slot %d to_weapon %lx to_definition %lx",
				actor, game_time_get(), unit_index, trace->generation, trace->slot,
				trace->weapon_index, trace->definition_index, slot, weapon_index, weapon->definition_index);
		}
		else trace->generation = 0;
		trace->reported_slots = 0;
	}
	trace->weapon_index = weapon_index;
	trace->definition_index = weapon->definition_index;
	trace->slot = slot;
}

/* The producer supplies its firing weapon, not whichever weapon is held when a
charged projectile finishes. Log only the first success per firing slot/generation. */
void network_test_suite_weapon_projectile_created(long owner_unit, long weapon_index, long projectile_index)
{
	long player_index, *players;
	struct player_datum *player;
	struct unit_datum *unit;
	struct weapon_datum *weapon;
	struct object_datum *projectile;
	struct network_test_weapon_trace *trace;
	int actor, i;
	short slot;

	if (!network_test.suite_owned || network_test.mode != _network_test_host ||
		network_test.suite_network_players < 2 || network_test.suite_network_players > BENCHMARK_MP_MAXIMUM_PLAYERS ||
		owner_unit == NONE || weapon_index == NONE || projectile_index == NONE || game_connection() != _game_connection_network_server) return;
	player_index = player_index_from_unit_index(owner_unit);
	player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	if (!player || player->unit_index != owner_unit) return;
	actor = player->network_player_data.machine_index;
	if (actor < 1 || actor >= network_test.suite_network_players || player->local_player_index != NONE) return;
	players = machine_get_player_list(actor);
	if (!players || players[0] != player_index) return;
	for (i = 1; i < 4; i++) if (players[i] != NONE) return;
	unit = unit_try_and_get(owner_unit);
	weapon = weapon_try_and_get(weapon_index);
	projectile = object_try_and_get(projectile_index);
	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || !weapon || !projectile ||
		projectile->object.type != _object_type_projectile || projectile->object.owner_object_index != owner_unit ||
		!TEST_FLAG(weapon->item.flags, _item_attached_to_unit_bit) || weapon->object.owner_player_index != player_index ||
		(weapon->object.parent_object_index != NONE && weapon->object.parent_object_index != owner_unit)) return;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		if (unit->unit.weapon_object_indices[slot] == weapon_index) break;
	if (slot == MAXIMUM_WEAPONS_PER_UNIT) return;
	network_test_suite_observe_weapon(actor, owner_unit);
	trace = &network_test.suite_weapon_trace[actor];
	if (!trace->ready || trace->unit_index != owner_unit) return;
	if (trace->projectiles != UINT32_MAX) trace->projectiles++;
	if (!trace->generation || TEST_FLAG(trace->reported_slots, slot)) return;
	SET_FLAG(trace->reported_slots, slot, TRUE);
	platform_log("network test: bot actual projectile actor %d tick %ld unit %lx generation %lu firing_slot %d weapon %lx definition %lx projectile %lx current_slot %d current_weapon %lx current_definition %lx matched %d total %u",
		actor, game_time_get(), owner_unit, trace->generation, slot, weapon_index, weapon->definition_index,
		projectile_index, trace->slot, trace->weapon_index, trace->definition_index,
		weapon_index == trace->weapon_index, trace->projectiles);
}

/* Workload revision 2 uses real, distinct carried weapons. Never retain a slot
across a respawn: the salted unit and both owned weapon objects must still match. */
static short network_test_suite_bot_weapon(int actor, long player_index, long tick)
{
	static const char *const names[] = {
		"weapons\\pistol\\pistol", "weapons\\assault rifle\\assault rifle",
		"weapons\\plasma rifle\\plasma rifle", "weapons\\plasma pistol\\plasma pistol",
		"weapons\\needler\\needler", "weapons\\shotgun\\shotgun"
	};
	struct player_datum *player = player_try_and_get(player_index);
	struct unit_datum *unit = player ? unit_try_and_get(player->unit_index) : NULL;
	long definitions[2];
	short slots[2] = { NONE, NONE }, slot, selected;
	int n;

	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		return NONE;
	/* Resolve the map's tags once, outside the per-actor tick hot path. */
	if (!network_test.suite_weapon_definitions_ready)
	{
		for (n = 0; n < 6; n++)
		{
			int previous;

			network_test.suite_weapon_definitions[n] = tag_loaded(WEAPON_DEFINITION_TAG, names[n]);
			if (network_test.suite_weapon_definitions[n] == NONE)
			{
				platform_log("network test: bot weapon workload 2 missing definition %s", names[n]);
				return -2;
			}
			for (previous = 0; previous < n; previous++)
				if (network_test.suite_weapon_definitions[previous] == network_test.suite_weapon_definitions[n]) return -2;
		}
		network_test.suite_weapon_definitions_ready = 1;
	}
	for (n = 0; n < 2; n++)
		definitions[n] = network_test.suite_weapon_definitions[(actor + n * 3) % 6];
	if (!network_test.suite_weapon_ready[actor] || network_test.suite_weapon_units[actor] != player->unit_index)
	{
		network_test_suite_clear_bot_weapons(player->unit_index);
		for (n = 0; n < 2; n++)
		{
			struct object_placement_data placement;
			long weapon_index;

			object_placement_data_new(&placement, definitions[n], player->unit_index);
			weapon_index = object_new(&placement);
			if (weapon_index == NONE)
			{
				network_test_suite_clear_bot_weapons(player->unit_index);
				return -2;
			}
			if (!unit_add_weapon_to_inventory(player->unit_index, weapon_index, _unit_add_weapon_starting))
			{
				object_delete_immediately(weapon_index);
				network_test_suite_clear_bot_weapons(player->unit_index);
				return -2;
			}
		}
		network_test.suite_weapon_units[actor] = player->unit_index;
		network_test.suite_weapon_ready[actor] = 1;
		platform_log("network test: bot weapon workload 2 actor %d unit %lx definitions %lx/%lx (%s / %s)",
			actor, player->unit_index, definitions[0], definitions[1], tag_get_name(definitions[0]), tag_get_name(definitions[1]));
	}
	/* Object creation can compact the pool; do not retain the old unit pointer. */
	unit = unit_try_and_get(player->unit_index);
	if (!unit) return -2;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		struct weapon_datum *weapon = weapon_try_and_get(unit->unit.weapon_object_indices[slot]);

		/* Stowed weapons are detached; item_in_unit_inventory records their
		owner and inventory flag, while the readied weapon is attached. */
		if (!weapon || !TEST_FLAG(weapon->item.flags, _item_attached_to_unit_bit) ||
			weapon->object.owner_player_index != unit->unit.player_index ||
			(weapon->object.parent_object_index != NONE && weapon->object.parent_object_index != player->unit_index)) continue;
		for (n = 0; n < 2; n++)
			if (weapon->definition_index == definitions[n]) slots[n] = slot;
	}
	if (slots[0] == NONE || slots[1] == NONE || slots[0] == slots[1]) return -2;
	/* Coprime actor offsets spread changes throughout the six-second period.
	Hold the requested slot through its ready animation, rather than a one-tick pulse. */
	selected = (short)(((tick + actor * 37) / (6 * TICKS_PER_SECOND)) % 2);
	network_test_suite_observe_weapon(actor, player->unit_index);
	return slots[selected];
}

void network_test_suite_synthetic_input_tick(long tick)
{
	long player_indices[BENCHMARK_MP_MAXIMUM_PLAYERS];
	int target = network_test.suite_network_players, i, duplicate_machine = NONE, prepared;
	unsigned short control_flags[DISTRIBUTED_INPUT_HISTORY] = { 0 };

	if (!network_test.suite_owned || network_test.mode != _network_test_host || network_test.players_wanted != 1 || target < 2 || target > BENCHMARK_MP_MAXIMUM_PLAYERS ||
		game_connection() != _game_connection_network_server || tick < 0)
		return;
	{
		prepared = benchmark_mp_arena_tick(tick);
		if (prepared < 0)
		{
			platform_log("network test: arena preparation failed at tick %ld after %lu setup walking batches", tick, network_test.suite_setup_input_ticks);
			network_test.mode = _network_test_off;
			return;
		}
	}
	/* Validate the complete batch before mutating any queue. A partial roster
	cannot continue as a smaller successful workload. */
	for (i = 1; i < target; i++)
	{
		long *players = machine_get_player_list(i);
		int j;

		if (players && players[1] != NONE)
			goto failed;
		if (!players || players[0] == NONE || !update_server_player_queue_ready(players[0]))
		{
			/* Registration can still be pending before every unit exists. Keep
			this batch atomic and let the arena's finite readiness deadline apply. */
			if (!prepared) return;
			goto failed;
		}
		player_indices[i] = players[0];
		for (j = 1; j < i; j++)
			if (player_indices[j] == player_indices[i])
			{
				duplicate_machine = j;
				goto failed;
			}
	}
	/* Normal movement vacates occupied stock starts while later units spawn.
	Setup input is not measured activity; the arena gate and lead-in still apply. */
	if (!prepared)
		network_test.suite_setup_input_ticks++;
	else if (network_test.suite_setup_input_ticks && !network_test.suite_setup_input_reported)
	{
		platform_log("network test: arena ready at tick %ld after %lu setup walking batches for %d remote players; setup excluded from measured activity",
			tick, network_test.suite_setup_input_ticks, target - 1);
		network_test.suite_setup_input_reported = TRUE;
	}
	for (i = 1; i < target; i++)
	{
		struct player_action action = { 0 };

		action.desired_facing.yaw = (real)((tick + i * 17) % 360) * 0.01f;
		action.throttle.j = 0.75f;
		action.primary_trigger = prepared && tick % 90 < 24 ? 1.0f : 0.0f;
		if (prepared && benchmark_mp_arena_enabled())
		{
			memset(&action, 0, sizeof(action));
			benchmark_mp_arena_action(i, tick, &action);
		}
		SET_FLAG(action.control_flags, _unit_control_weapon_primary_trigger_bit, action.primary_trigger > 0.0f);
		action.desired_weapon_index = prepared ? network_test_suite_bot_weapon(i, player_indices[i], tick) : NONE;
		if (action.desired_weapon_index < NONE)
		{
			platform_log("network test: bot weapon workload 2 inventory failed actor %d tick %ld", i, tick);
			network_test.mode = _network_test_off;
			return;
		}
		action.desired_grenade_index = NONE;
		action.desired_zoom_level = NONE;
		update_server_handle_distributed_input(player_indices[i], tick, &action, control_flags, DISTRIBUTED_INPUT_HISTORY);
	}
	return;
failed:
	{
		long *players = machine_get_player_list(i);

		platform_log("network test: incomplete synthetic input roster at tick %ld machine %d player %ld second %ld queue ready %d duplicate machine %d",
			tick, i, players ? players[0] : NONE, players ? players[1] : NONE,
			players && players[0] != NONE ? update_server_player_queue_ready(players[0]) : 0, duplicate_machine);
	}
	network_test.mode = _network_test_off;
}

/* the variant at the index of the list (copied to name), FALSE past its end */
static boolean network_test_variant(
	short index,
	char *name,
	size_t size)
{
	char const *variant = network_test.variant_name;

	for (; index > 0 && variant; index--)
	{
		variant = strchr(variant, ',');
		if (variant)
			variant++;
	}
	if (!variant || !*variant)
		return FALSE;
	if (name)
	{
		size_t length = strcspn(variant, ",");

		snprintf(name, size, "%.*s", (int)length, variant);
	}
	return TRUE;
}

static void network_test_read_settings(
	void)
{
	char const *setting = config_string("debug.network_test");

	network_test.checked = TRUE;
	if (!strncmp(setting, "host:", 5) && setting[5])
	{
		char *colon;

		network_test.mode = _network_test_host;
		snprintf(network_test.map_name, sizeof(network_test.map_name), "%s", setting + 5);
		snprintf(network_test.variant_name, sizeof(network_test.variant_name), "slayer");
		colon = strchr(network_test.map_name, ':');
		if (colon)
		{
			*colon = 0;
			snprintf(network_test.variant_name, sizeof(network_test.variant_name), "%s", colon + 1);
		}
	}
	else if (!strcmp(setting, "join"))
	{
		network_test.mode = _network_test_join;
	}
	else if (!strcmp(setting, "browse"))
	{
		network_test.mode = _network_test_browse;
	}
	network_test.players_wanted = (short)config_integer("debug.network_test_players");
	network_test.public_game = config_boolean("debug.network_test_public") != 0;
	network_test.start_delay = (real)config_real("debug.network_test_start");
	network_test.kill_interval = (real)config_real("debug.network_test_kill");
	network_test.shoot_interval = (real)config_real("debug.network_test_shoot");
	network_test.vehicle_time = (real)config_real("debug.network_test_vehicle");
	network_test.pickup_time = (real)config_real("debug.network_test_pickup");
	snprintf(network_test.pickup_weapon, sizeof(network_test.pickup_weapon), "%s",
		config_string("debug.network_test_pickup_weapon"));
	network_test.score_to_win = (long)config_integer("debug.network_test_score");
	if (network_test.mode != _network_test_off)
		platform_log("network test: %s", setting);
}

/* appends to a line, cut short when it is full */
static void network_test_append(
	char *line,
	int size,
	int *length,
	char const *format,
	...)
{
	va_list arguments;
	int written;

	if (*length >= size - 1)
		return;
	va_start(arguments, format);
	written = vsnprintf(line + *length, (size_t)(size - *length), format, arguments);
	va_end(arguments);
	if (written > 0)
		*length = MIN(*length + written, size - 1);
}

/* every player's unit, as this machine sees it */
static void network_test_log_players(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	char line[4096];
	int length = 0, roster = 0, spawned = 0;

	line[0] = 0;
	/* (each player's name, once a game: for tests that name a player, such
	as the host's ban command) */
	{
		static wchar_t named[HALO_PORT_MAXIMUM_NETWORK_PLAYERS][12];

		if (game_time_get() < 2 * TICKS_PER_SECOND)
			csmemset(named, 0, sizeof(named));
		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
			char name[13];
			short index;

			if (absolute_index >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS ||
				!csmemcmp(named[absolute_index], player->name, sizeof(named[absolute_index])))
			{
				continue;
			}
			csmemcpy(named[absolute_index], player->name, sizeof(named[absolute_index]));
			for (index = 0; index < 12 && player->name[index]; index++)
				name[index] = player->name[index] >= 32 && player->name[index] < 127 ? (char)player->name[index] : '?';
			name[index] = 0;
			platform_log("network test: player %ld is named %s", absolute_index, name);
		}
	}
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		roster++;
		spawned += player->unit_index != NONE;
		if (player->unit_index != NONE)
		{
			struct object_datum *object = object_get(player->unit_index);
			struct unit_datum *unit = unit_get(player->unit_index);
			short slot;

			/* (riding: where its vehicle is) */
			struct object_datum *placed = object->object.parent_object_index != NONE ?
				object_get(object->object.parent_object_index) : object;

			network_test_append(line, (int)sizeof(line), &length, " player %ld: (%.3f %.3f %.3f) h%.2f/%.2f%s%s g%d/%d w",
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index), placed->object.position.x,
				placed->object.position.y, placed->object.position.z, object->object.body_vitality,
				object->object.shield_vitality, placed != object ? " riding" : "",
				TEST_FLAG(unit->unit.flags, _unit_active_camouflaged_bit) ? " camo" : "",
				unit->unit.grenade_counts[0], unit->unit.grenade_counts[1]);
			/* where it aims (yaw and pitch, degrees), its animation state and
			how hard it is moving */
			network_test_append(line, (int)sizeof(line), &length, " a%.0f/%.0f f%.0f l%.0f/%.0f as%d/%d st%d thr%.2f",
				atan2(unit->unit.aiming_vector.j, unit->unit.aiming_vector.i) * 57.29578,
				asin(PIN(unit->unit.aiming_vector.k, -1.0f, 1.0f)) * 57.29578,
				atan2(object->object.forward.j, object->object.forward.i) * 57.29578,
				atan2(unit->unit.looking_vector.j, unit->unit.looking_vector.i) * 57.29578,
				asin(PIN(unit->unit.looking_vector.k, -1.0f, 1.0f)) * 57.29578,
				(int)unit->unit.animation.aiming_screen_index, (int)unit->unit.animation.looking_screen_index,
				(int)unit->unit.animation.state,
				sqrt(unit->unit.throttle.i * unit->unit.throttle.i + unit->unit.throttle.j * unit->unit.throttle.j));
			if (network_test.suite_owned)
				network_test_append(line, (int)sizeof(line), &length, " current%d fire%d pull%.2f",
					(int)unit->unit.current_weapon_index,
					(int)TEST_FLAG(unit->unit.control_flags, _unit_control_weapon_primary_trigger_bit),
					unit->unit.primary_trigger);
			for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
			{
				long weapon_index = unit->unit.weapon_object_indices[slot];

				if (weapon_index != NONE)
				{
					struct weapon_datum *weapon = weapon_get(weapon_index);

					network_test_append(line, (int)sizeof(line), &length, " %lx:%d",
						(unsigned long)weapon->definition_index & 0xFFFF, weapon->weapon.magazines[0].rounds_total +
						weapon->weapon.magazines[0].rounds_loaded);
				}
			}
		}
		else
		{
			network_test_append(line, (int)sizeof(line), &length, " player %ld: dead",
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index));
		}
		/* the game type's score and the kills and deaths */
		network_test_append(line, (int)sizeof(line), &length, " s%ld k%d d%d f%d t%ld m%d",
			game_engine && game_engine->get_player_score ?
				game_engine->get_player_score(iterator.datum_index, _get_score_individual) : -1L,
			player->statistics.kills[0], player->statistics.deaths, player->statistics.friendly_fire_kills, (long)player->team_index,
			(int)player->network_player_data.machine_index);
		if (network_test.suite_owned)
			network_test_append(line, (int)sizeof(line), &length, " reposition%lu energy_projectiles%u",
				network_test_suite_repositions(player->local_player_index != NONE ? player->local_player_index : player->network_player_data.machine_index),
				network_test_suite_energy_projectiles(player->local_player_index != NONE ? player->local_player_index : player->network_player_data.machine_index));
		if (network_test.suite_owned && network_test.suite_network_players)
		{
			/* One bounded record per actor keeps complete rosters out of the
			shared platform log buffer's truncation path. */
			platform_log("network test: tick %ld actor%s", game_time_get(), line);
			length = 0;
			line[0] = 0;
		}
	}
	if (network_test.suite_owned && network_test.suite_network_players)
	{
		unsigned long hits, kills;
		network_test_suite_scripted_counts(&hits, &kills);
		network_test_append(line, (int)sizeof(line), &length, " roster %d spawned %d requested %d scripted_hits %lu scripted_kills %lu",
			roster, spawned, network_test.suite_network_players, hits, kills);
	}
	{
		long sent, received, corrections;

		struct object_iterator objects;
		long ground_items = 0;

		object_iterator_new(&objects, _object_mask_weapon | _object_mask_equipment, 0);
		while (object_iterator_next(&objects))
		{
			struct item_datum *item = item_get(objects.index);

			if (item->object.parent_object_index == NONE &&
				TEST_FLAG(item->object.flags, _object_connected_to_map_bit) &&
				!TEST_FLAG(item->item.flags, _item_attached_to_unit_bit))
			{
				ground_items++;
			}
		}
		network_distributed_statistics(&sent, &received, &corrections);
		long creates, deletes, failures, removed;

		long sent_reports, dealt_reports, rejected_reports, replayed_events;

		network_distributed_item_statistics(&creates, &deletes, &failures, &removed);
		network_damage_statistics(&sent_reports, &dealt_reports, &rejected_reports, &replayed_events);
		/* this machine's player and where its camera is (a player that never
		spawns leaves it where it began) */
		long local_player_index = local_player_get_player_index(0);
		struct observer_result const *camera = observer_get_camera(0);

		platform_log("network test: tick %ld%s | items %ld (+%ld -%ld !%ld x%ld) | %s to %ld | sent %ld received %ld corrected %ld"
			" | hits %ld dealt %ld rejected %ld replayed %ld | local %ld camera (%.1f %.1f %.1f) respawn %ld",
			game_time_get(), line, ground_items, creates, deletes, failures, removed,
			game_engine_can_score() ? "playing" : "game over",
			game_engine_running() ? (long)game_engine_get_variant()->universal_variant.score_to_win : 0L,
			sent, received, corrections,
			sent_reports, dealt_reports, rejected_reports, replayed_events,
			local_player_index == NONE ? -1L : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(local_player_index),
			camera ? camera->position.x : 0.0f, camera ? camera->position.y : 0.0f, camera ? camera->position.z : 0.0f,
			local_player_index == NONE ? 0L : (long)player_get(local_player_index)->respawn_timer);
	}
}

/* each of this machine's players hits the next player with their weapon's
projectile, as its impact would, when it is within the projectile's reach
(as a real shot is: the host checks it) */
static void network_test_shoot(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct data_iterator targets;
		struct player_datum *target = NULL;
		struct player_datum *candidate;
		struct unit_datum *unit;
		long weapon_index;
		struct weapon_definition *weapon;
		struct weapon_trigger_definition *trigger;
		long damage_index = NONE;
		real reach = 0.0f;
		boolean melee;
		struct damage_data damage;
		struct object_datum *target_object;
		real_vector3d direction;

		if (player->local_player_index == NONE || player->unit_index == NONE)
			continue;
		data_iterator_new(&targets, player_data);
		while ((candidate = (struct player_datum *)data_iterator_next(&targets)) != NULL)
		{
			if (candidate != player && candidate->unit_index != NONE)
			{
				target = candidate;
				break;
			}
		}
		unit = unit_get(player->unit_index);
		if (!target || unit->unit.current_weapon_index == NONE ||
			TEST_FLAG(object_get(target->unit_index)->object.damage_flags, _object_dead_bit))
		{
			continue;
		}
		weapon_index = unit->unit.weapon_object_indices[unit->unit.current_weapon_index];
		if (weapon_index == NONE)
			continue;
		weapon = weapon_definition_get(object_get(weapon_index)->definition_index);
		if (weapon->weapon.triggers.count > 0)
		{
			trigger = TAG_BLOCK_GET_ELEMENT(&weapon->weapon.triggers, 0, struct weapon_trigger_definition);
			if (trigger->projectile.index != NONE)
			{
				struct projectile_definition *projectile = projectile_definition_get(trigger->projectile.index);

				damage_index = projectile->projectile.impact_damage.index;
				/* (how far it flies: its range, or at its speed for as long as
				its timer runs; none for no bound, network_damage.c) */
				if (projectile->projectile.maximum_range > 0.0f)
					reach = projectile->projectile.maximum_range;
				else if (projectile->projectile.detonation_timer_starts == 0)
				{
					reach = projectile->projectile.timer_upper_bound * TICKS_PER_SECOND *
						MAX(projectile->projectile.initial_velocity, projectile->projectile.final_velocity);
				}
			}
		}
		/* (else its melee: a blow's epicenter is its striker's, as
		unit_cause_player_melee_damage has it) */
		melee = damage_index == NONE;
		if (melee)
			damage_index = weapon->weapon.melee_attack_damage.index;
		if (damage_index == NONE)
			continue;
		target_object = object_get(target->unit_index);
		/* (a blow reaches only a target at hand, as the host checks) */
		if (melee)
		{
			real dx = target_object->object.position.x - unit->object.position.x;
			real dy = target_object->object.position.y - unit->object.position.y;
			real dz = target_object->object.position.z - unit->object.position.z;

			if (dx * dx + dy * dy + dz * dz > 1.5f * 1.5f)
				continue;
		}
		/* (a shot reaches no further than its projectile flies, with a margin
		for how far the target is from where the shot started) */
		else if (reach > 0.0f)
		{
			real dx = target_object->object.position.x - unit->object.position.x;
			real dy = target_object->object.position.y - unit->object.position.y;
			real dz = target_object->object.position.z - unit->object.position.z;

			if (dx * dx + dy * dy + dz * dz > 0.9f * reach * 0.9f * reach)
				continue;
		}
		damage_data_new(&damage, damage_index);
		damage.owner_player_index = iterator.datum_index;
		damage.owner_object_index = player->unit_index;
		damage.owner_team_index = unit->object.owner_team_index;
		damage.origin = melee ? unit->object.bounding_sphere_center : target_object->object.position;
		damage.epicenter = melee ? unit->object.bounding_sphere_center : target_object->object.position;
		direction.i = target_object->object.position.x - unit->object.position.x;
		direction.j = target_object->object.position.y - unit->object.position.y;
		direction.k = target_object->object.position.z - unit->object.position.z;
		normalize3d(&direction);
		damage.direction = direction;
		damage.scale = 1.0f;
		scenario_location_from_point(&damage.location, &damage.epicenter);
		if (network_test.suite_owned) network_test.suite_scripted_hits++;
		object_cause_damage(&damage, target->unit_index, NONE, NONE, NONE, NULL);
		platform_log("network test: player %ld shoots player %ld",
			(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index), (long)(target - (struct player_datum *)player_data->data));
	}
}

/* the host seats the last player as the nearest vehicle's driver, or out */
/* the host brings the players far from the first (on foot) near it, so
that they are within their weapons' reach (network_test_shoot shoots only
so near, as a player does); the host moving a client's player, as a
teleporter does */
static void network_test_gather(
	boolean leave_last)
{
	/* Signed arenas own their staging and recovery, with per-actor counters. */
	if (network_test.suite_owned && benchmark_mp_arena_enabled())
		return;
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *first = NULL;
	struct player_datum *last = NULL;
	short count = 0;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (!first)
			first = player;
		last = player;
	}
	if (!first || first->unit_index == NONE)
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct object_datum const *center = object_get(first->unit_index);
		struct object_datum *unit;
		real_point3d position;
		real dx, dy, dz;

		if (player == first || player->unit_index == NONE || (leave_last && player == last))
			continue;
		count++;
		unit = object_get(player->unit_index);
		if (unit->object.parent_object_index != NONE || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
			continue;
		dx = unit->object.position.x - center->object.position.x;
		dy = unit->object.position.y - center->object.position.y;
		dz = unit->object.position.z - center->object.position.z;
		if (dx * dx + dy * dy + dz * dz <= 20.0f * 20.0f)
			continue;
		/* (beside it, where the map is open: at its feet and its head) */
		{
			static real const offsets[][2] = { { 1.5f, 0.0f }, { -1.5f, 0.0f }, { 0.0f, 1.5f }, { 0.0f, -1.5f },
				{ 1.5f, 1.5f }, { -1.5f, -1.5f }, { 1.5f, -1.5f }, { -1.5f, 1.5f } };
			short try_index;

			for (try_index = 0; try_index < (short)NUMBEROF(offsets); try_index++)
			{
				short index = (short)((count + try_index) % (short)NUMBEROF(offsets));
				struct location feet, head;
				real_point3d above;

				position = center->object.position;
				position.x += offsets[index][0];
				position.y += offsets[index][1];
				position.z += 0.2f;
				above = position;
				above.z += 0.7f;
				scenario_location_from_point(&feet, &position);
				scenario_location_from_point(&head, &above);
				if (feet.cluster_index != NONE && head.cluster_index != NONE)
					break;
			}
			if (try_index >= (short)NUMBEROF(offsets))
				continue;
		}
		object_set_position(player->unit_index, &position, NULL, NULL);
		if (network_test.suite_owned)
		{
			int actor = player->local_player_index != NONE ? player->local_player_index : player->network_player_data.machine_index;
			if (actor >= 0 && actor < BENCHMARK_MP_MAXIMUM_PLAYERS) network_test.suite_repositions[actor]++;
		}
		platform_log("network test: the host brings player %ld near the first",
			(long)(player - (struct player_datum *)player_data->data));
	}
}

static void network_test_vehicle(
	boolean enter)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *last = NULL;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		last = player;
	if (!last || last->unit_index == NONE)
		return;
	if (!enter)
	{
		if (object_get(last->unit_index)->object.parent_object_index != NONE)
		{
			unit_exit_seat_end(last->unit_index);
			platform_log("network test: the last player leaves the vehicle");
		}
		return;
	}
	{
		struct object_iterator vehicles;
		long nearest_index = NONE;
		real nearest_distance = 0.0f;
		real_point3d const *position = &object_get(last->unit_index)->object.position;
		short seat_index;

		object_iterator_new(&vehicles, _object_mask_vehicle, 0);
		while (object_iterator_next(&vehicles))
		{
			real_point3d const *vehicle_position = &object_get(vehicles.index)->object.position;
			real distance = distance_squared3d(position, vehicle_position);

			if (nearest_index == NONE || distance < nearest_distance)
			{
				nearest_index = vehicles.index;
				nearest_distance = distance;
			}
		}
		if (nearest_index == NONE)
		{
			platform_log("network test: no vehicle");
			return;
		}
		for (seat_index = 0; seat_index < unit_definition_get(object_get(nearest_index)->definition_index)->unit.seats.count; seat_index++)
		{
			if (unit_seat_is_driver(nearest_index, seat_index) &&
				unit_enter_seat(last->unit_index, nearest_index, seat_index))
			{
				platform_log("network test: the last player drives vehicle %lx", nearest_index);
				return;
			}
		}
		platform_log("network test: the last player cannot drive vehicle %lx", nearest_index);
	}
}

/* both machines stand the last player at the driver's entrance of the
nearest vehicle no one rides (a joining machine's player then holds the
action button, as a player getting in does) */
static void network_test_vehicle_approach(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *last = NULL;
	struct object_iterator vehicles;
	long nearest_index = NONE;
	real nearest_distance = 0.0f;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		last = player;
	if (!last || last->unit_index == NONE || object_get(last->unit_index)->object.parent_object_index != NONE)
		return;
	object_iterator_new(&vehicles, _object_mask_vehicle, 0);
	while (object_iterator_next(&vehicles))
	{
		struct unit_datum *vehicle = unit_get(vehicles.index);
		/* (the same vehicle on every machine: the lowest index) */
		real distance = (real)DATUM_INDEX_TO_ABSOLUTE_INDEX(vehicles.index);

		if (vehicle->unit.driver_object_index != NONE || TEST_FLAG(vehicle->object.damage_flags, _object_dead_bit))
			continue;
		if (nearest_index == NONE || distance < nearest_distance)
		{
			nearest_index = vehicles.index;
			nearest_distance = distance;
		}
	}
	if (nearest_index != NONE)
	{
		short seat_index;

		for (seat_index = 0; seat_index < unit_definition_get(object_get(nearest_index)->definition_index)->unit.seats.count;
			seat_index++)
		{
			real_point3d entrance;
			real_point3d seat;

			if (unit_seat_is_driver(nearest_index, seat_index) &&
				unit_get_seat_entrance_point(last->unit_index, nearest_index, seat_index, &entrance, &seat, NULL))
			{
				object_set_position(last->unit_index, &entrance, NULL, NULL);
				platform_log("network test: the last player stands at vehicle %lx's driver's entrance (%.2f %.2f %.2f)",
					nearest_index, entrance.x, entrance.y, entrance.z);
				return;
			}
		}
	}
	platform_log("network test: no vehicle to get into");
}

/* the host gives the last player a second weapon, lying about */
static void network_test_second_weapon(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *last = NULL;
	struct object_iterator weapons;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		last = player;
	if (!last || last->unit_index == NONE)
		return;
	object_iterator_new(&weapons, _object_mask_weapon, 0);
	while (object_iterator_next(&weapons))
	{
		struct object_datum *weapon = object_get(weapons.index);
		struct unit_datum *unit = unit_get(last->unit_index);
		long current_index = unit->unit.weapon_object_indices[0];

		if (weapon->object.parent_object_index == NONE && TEST_FLAG(weapon->object.flags, _object_connected_to_map_bit) &&
			(current_index == NONE || object_get(current_index)->definition_index != weapon->definition_index) &&
			unit_add_weapon_to_inventory(last->unit_index, weapons.index, TRUE))
		{
			platform_log("network test: the last player takes a second weapon (%lx)", weapon->definition_index);
			/* (and camouflage, as a powerup gives) */
			player_handle_powerup(DATUM_INDEX_NEW(last - (struct player_datum *)player_data->data, last->identifier),
				_player_powerup_active_camouflage, 10 * TICKS_PER_SECOND);
			return;
		}
	}
}

/* both machines stand the last player on the first weapon lying about that
it does not carry (the same object on both: a client's own player is where
it has it, within a tolerance) */
static void network_test_pickup(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *last = NULL;
	struct object_iterator weapons;
	long nearest_index = NONE;
	real nearest_distance = 0.0f;
	struct unit_datum *unit;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		last = player;
	if (!last || last->unit_index == NONE)
		return;
	unit = unit_get(last->unit_index);
	object_iterator_new(&weapons, _object_mask_weapon, 0);
	while (object_iterator_next(&weapons))
	{
		struct object_datum *weapon = object_get(weapons.index);
		short slot;
		boolean carried = FALSE;
		real distance;

		if (weapon->object.parent_object_index != NONE ||
			!TEST_FLAG(weapon->object.flags, _object_connected_to_map_bit))
		{
			continue;
		}
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long carried_index = unit->unit.weapon_object_indices[slot];

			carried |= carried_index != NONE && object_get(carried_index)->definition_index == weapon->definition_index;
		}
		if (network_test.pickup_weapon[0] && !strstr(tag_get_name(weapon->definition_index), network_test.pickup_weapon))
			continue;
		distance = (real)DATUM_INDEX_TO_ABSOLUTE_INDEX(weapons.index);
		if (!carried && (nearest_index == NONE || distance < nearest_distance))
		{
			nearest_index = weapons.index;
			nearest_distance = distance;
		}
	}
	if (nearest_index != NONE)
	{
		real_point3d position = object_get(nearest_index)->object.position;

		position.z += 0.1f;
		object_set_position(last->unit_index, &position, NULL, NULL);
		platform_log("network test: the last player stands on weapon %lx (%lx)", nearest_index,
			object_get(nearest_index)->definition_index);
	}
}

static int network_test_suite_combat_ready(void)
{
	return !network_test.suite_owned || !benchmark_mp_arena_enabled() || benchmark_mp_arena_combat_ready();
}

void network_test_update(
	boolean main_menu_loaded,
	real seconds)
{
	if (!network_test.checked)
		network_test_read_settings();
	if (network_test.mode == _network_test_off)
		return;

	/* the game running: report (from the start of each game: the next
	game's time starts over) */
	if (game_in_progress() && game_time_get() < network_test.logged_time)
		network_test.logged_time = 0;
	if (game_in_progress() && !main_menu_loaded && game_time_get() - network_test.logged_time >= TICKS_PER_SECOND)
	{
		network_test.logged_time = game_time_get();
		network_test_log_players();
		if (network_test_suite_combat_ready() && network_test.shoot_interval > 0.0f &&
			game_time_get() % MAX(1, (long)(network_test.shoot_interval * TICKS_PER_SECOND)) < TICKS_PER_SECOND)
		{
			network_test_shoot();
		}
		/* (the host brings the players near a second before they shoot;
		not the last while the vehicle test has it) */
		if (network_test_suite_combat_ready() && network_test.mode == _network_test_host && network_test.shoot_interval > 0.0f &&
			(game_time_get() + TICKS_PER_SECOND) % MAX(1, (long)(network_test.shoot_interval * TICKS_PER_SECOND)) <
				TICKS_PER_SECOND)
		{
			long vehicle_time = (long)(network_test.vehicle_time * TICKS_PER_SECOND);

			network_test_gather(network_test.vehicle_time > 0.0f && game_time_get() >= vehicle_time - 2 * TICKS_PER_SECOND);
		}
		if (network_test.pickup_time > 0.0f)
		{
			long pickup_time = (long)(network_test.pickup_time * TICKS_PER_SECOND);

			/* (two weapons first: picking up a third swaps) */
			if (network_test.mode == _network_test_host && game_time_get() >= pickup_time - 3 * TICKS_PER_SECOND &&
				game_time_get() - (pickup_time - 3 * TICKS_PER_SECOND) < TICKS_PER_SECOND)
			{
				network_test_second_weapon();
			}

			if (game_time_get() >= pickup_time && game_time_get() - pickup_time < TICKS_PER_SECOND)
			{
				network_test_pickup();
			}
			/* (standing there for four seconds, the button held from a second
			on) */
			if (network_test.mode == _network_test_join)
			{
				boolean hold = game_time_get() >= pickup_time &&
					game_time_get() < pickup_time + 4 * TICKS_PER_SECOND;

				test_input_hold_action(hold);
			}
		}
		if (network_test.mode == _network_test_host && network_test.vehicle_time > 0.0f)
		{
			long enter_time = (long)(network_test.vehicle_time * TICKS_PER_SECOND);

			if (game_time_get() >= enter_time && game_time_get() - enter_time < TICKS_PER_SECOND)
				network_test_vehicle(TRUE);
			if (game_time_get() >= enter_time + 15 * TICKS_PER_SECOND &&
				game_time_get() - enter_time - 15 * TICKS_PER_SECOND < TICKS_PER_SECOND)
			{
				network_test_vehicle(FALSE);
			}
		}
		/* ... and 20 seconds on, the last player getting in as a player does:
		at the entrance, the button held for three seconds */
		if (network_test.vehicle_time > 0.0f)
		{
			long approach_time = (long)(network_test.vehicle_time * TICKS_PER_SECOND) + 20 * TICKS_PER_SECOND;

			if (game_time_get() >= approach_time && game_time_get() - approach_time < TICKS_PER_SECOND)
				network_test_vehicle_approach();
			if (network_test.mode == _network_test_join && network_test.pickup_time <= 0.0f)
			{
				test_input_hold_action(game_time_get() >= approach_time + TICKS_PER_SECOND &&
					game_time_get() < approach_time + 4 * TICKS_PER_SECOND);
			}
		}
		/* debug.network_test_kill: the host kills the last player every so
		often, to test deaths and respawns reaching the clients */
		if (network_test_suite_combat_ready() && network_test.mode == _network_test_host && network_test.kill_interval > 0.0f &&
			game_time_get() % MAX(1, (long)(network_test.kill_interval * TICKS_PER_SECOND)) < TICKS_PER_SECOND)
		{
			struct data_iterator iterator;
			struct player_datum *player;
			struct player_datum *last = NULL;

			struct player_datum *first = NULL;
			long first_index = NONE;

			data_iterator_new(&iterator, player_data);
			while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			{
				if (!first)
				{
					first = player;
					first_index = iterator.datum_index;
				}
				last = player;
			}
			if (last && last != first && last->unit_index != NONE && first->unit_index != NONE)
			{
				/* killed by the first player: a kill that scores */
				platform_log("network test: the first player kills the last");
				if (network_test.suite_owned) network_test.suite_scripted_kills++;
				damage_kill_object_for_player(last->unit_index, first_index);
				/* and picks up a weapon lying about, and two grenades of each kind */
				{
					struct object_iterator objects;
					struct unit_datum *unit = unit_get(first->unit_index);
					long held_index = unit->unit.weapon_object_indices[0];
					long held_definition_index = held_index != NONE ? weapon_get(held_index)->definition_index : NONE;

					object_iterator_new(&objects, _object_mask_weapon, 0);
					while (object_iterator_next(&objects))
					{
						struct weapon_datum *weapon = weapon_get(objects.index);

						if (weapon->object.parent_object_index == NONE &&
							weapon->definition_index != held_definition_index)
						{
							if (unit_add_weapon_to_inventory(first->unit_index, objects.index, TRUE))
								platform_log("network test: the first player picks up a weapon");
							break;
						}
					}
					unit->unit.grenade_counts[0] = 2;
					unit->unit.grenade_counts[1] = 2;
				}
			}
		}
	}

	/* the next game of the list: once the scores have been shown a while,
	the host's button (the bots may press it first) */
	if (network_test.mode == _network_test_host && network_test.started && game_engine_running() &&
		!main_menu_loaded && !game_engine_can_score())
	{
		network_test.game_over = TRUE;
		if (game_engine_showing_postgame() && global_network_game_server_get())
		{
			network_test.postgame_seconds += seconds;
			if (network_test.postgame_seconds >= 3.0f && network_test_variant(network_test.variant_index + 1, NULL, 0))
			{
				network_test.postgame_seconds = 0.0f;
				network_game_server_reset_to_pregame(global_network_game_server_get());
			}
		}
	}
	/* (a joining machine's player: the other team again in the next game's
	lobby, whose variant may have teams where the last had none) */
	if (network_test.mode == _network_test_join && network_test.team_set && game_engine_running() &&
		!main_menu_loaded && !game_engine_can_score())
	{
		network_test.game_over = TRUE;
	}
	if (network_test.mode == _network_test_join && network_test.game_over && main_menu_loaded)
	{
		network_test.game_over = FALSE;
		network_test.team_set = FALSE;
		network_test.joined_seconds = 0.0f;
	}
	/* ... back in the lobby, set up as the first was */
	if (network_test.mode == _network_test_host && network_test.game_over && main_menu_loaded)
	{
		network_test.game_over = FALSE;
		network_test.postgame_seconds = 0.0f;
		if (network_test_variant(network_test.variant_index + 1, NULL, 0))
		{
			network_test.variant_index++;
			network_test.started = FALSE;
			network_test.map_set = FALSE;
			network_test.setup_seconds = 0.0f;
			network_test.menu_seconds = 0.0f;
			/* (as picking the next game's map does: the scores' map choice
			holds the countdown) */
			if (global_network_game_server_get())
				network_game_server_pause_countdown(global_network_game_server_get(), FALSE);
			platform_log("network test: the next game");
		}
	}

	if (!main_menu_loaded)
		return;
	network_test.menu_seconds += seconds;
	/* (the main menu settling first) */
	if (network_test.menu_seconds < 2.0f)
		return;

	switch (network_test.mode)
	{
	case _network_test_browse:
	{
		struct p2p_listing games[8];
		int count;

		if (!network_test.browsing)
		{
			network_test.browsing = TRUE;
			p2p_lobby_browse(TRUE);
			platform_log("network test: browsing the public games");
		}
		count = p2p_lobby_games(games, NUMBEROF(games));
		/* (the first that can be joined: no password, not failed) */
		while (count > 0 && (!games[0].invite[0] || games[0].failed))
		{
			memmove(games, games + 1, sizeof(*games) * (count - 1));
			count--;
		}
		if (count > 0 && p2p_join_invite(games[0].invite))
		{
			p2p_lobby_browse(FALSE);
			platform_log("network test: joining %s's game (%s) from the server browser", games[0].name, games[0].map);
			network_test.mode = _network_test_join;
		}
		break;
	}
	case _network_test_host:
		if (!network_test.set_up)
		{
			network_test.set_up = TRUE;
#ifdef HALO_IOS
			{
				/* (menu_functions.c) as Create Game > Internet, which this
				host stands in for, asks about internet play when it is off */
				extern void menu_internet_screen_opened(void);

				menu_internet_screen_opened();
			}
#endif
			main_set_multiplayer_map_name(network_test.map_name);
			player_ui_fast_setup_network_server();
			platform_log("network test: hosting %s", network_test.map_name);
			/* (in the server browser, as Create Game > Internet's PUBLIC) */
			if (network_test.public_game)
				p2p_set_hosting_public(TRUE);
		}
		else if (!network_test.started)
		{
			network_test.setup_seconds += seconds;
			/* the map (fast setup clears it), and a player for controller 1, as
			pressing A in the lobby adds one */
			if (!network_test.map_set && network_test.setup_seconds >= 1.0f && global_network_game_server_get())
			{
				char path[128];
				char variant_name[64];
				struct game_variant variant;

				/* (a level of its own path, else a multiplayer map's name) */
				if (strchr(network_test.map_name, '\\'))
					snprintf(path, sizeof(path), "%s", network_test.map_name);
				else
					snprintf(path, sizeof(path), "levels\\test\\%s\\%s", network_test.map_name, network_test.map_name);
				network_test_variant(network_test.variant_index, variant_name, sizeof(variant_name));
				if (!strcmp(variant_name, "coop"))
				{
					/* (the Map screen's co-op: menu_functions.c) */
					extern boolean ui_widget_port_cooperative_level_choose(char const *map_name, short difficulty);

					if (!ui_widget_port_cooperative_level_choose(path, _game_difficulty_level_normal))
						platform_log("network test: %s is not a campaign level, for co-op", path);
					platform_log("network test: game %d, co-op", network_test.variant_index + 1);
				}
				else
				{
					network_game_server_change_map_name(global_network_game_server_get(), path);
					/* the variant, as picking the game settings does */
					variant = *game_engine_get_variant_by_name(&variant, variant_name);
					platform_log("network test: game %d, %s", network_test.variant_index + 1, variant_name);
					/* debug.network_test_score: a short game, to test the next */
					if (network_test.score_to_win > 0)
						variant.universal_variant.score_to_win = network_test.score_to_win;
					player_ui_set_game_variant(&variant);
					network_game_server_change_game_variant(global_network_game_server_get(), &variant);
					if (network_test.suite_owned)
					{
						struct network_game *game = network_game_server_get_game(global_network_game_server_get());

						/* Suite limits belong to this private match, not the saved menu gametype. */
						game->variant_options.time_limit = network_test.suite_time_limit_minutes;
						if (!network_game_server_send_game_data_pregame(global_network_game_server_get()))
						{
							network_test.mode = _network_test_off;
							return;
						}
					}

				}
				network_test.map_set = TRUE;
			}
			if (!network_test.player_added && network_test.setup_seconds >= 2.0f && global_network_game_client_get())
				network_test.player_added = network_game_client_add_player(global_network_game_client_get(), 0);
			/* (split screen: the next controllers' players, a second apart) */
			if (network_test.player_added && network_test.players_added + 1 < network_test.players_wanted &&
				network_test.setup_seconds >= 3.0f + network_test.players_added && global_network_game_client_get())
			{
				network_game_client_add_player(global_network_game_client_get(), network_test.players_added + 1);
				network_test.players_added++;
			}
			if (network_test.suite_owned && network_test.players_wanted == 1 && network_test.map_set)
			{
				int prepared = network_test_suite_prepare_synthetic_players(global_network_game_server_get());

				if (prepared < 0 || (!prepared && network_test.setup_seconds >= network_test.start_delay))
				{
					platform_log("network test: synthetic player setup failed%s",
						prepared == 0 ? " waiting for local player registration" : "");
					network_test.mode = _network_test_off;
					return;
				}
				if (!prepared)
					return;
			}
			if (network_test.setup_seconds >= network_test.start_delay)
			{
				network_test.started = TRUE;
				network_game_client_request_immediate_start();
				platform_log("network test: starting the game");
			}
		}
		break;
	case _network_test_join:
		/* (the main menu's own start, main_menu_initialize, can come after this
		search began: it disposes the client and clears the players joined to
		multiplayer. The search then starts over) */
		if (network_test.set_up && !network_test.joined && !global_network_game_client_get())
		{
			network_test.set_up = FALSE;
			platform_log("network test: the search's client was disposed; searching again");
		}
		if (!network_test.set_up)
		{
			network_test.set_up = TRUE;
			dispose_global_network_game_client();
			dispose_global_network_game_server();
			if (create_global_network_game_client())
			{
				game_connection_set(_game_connection_network_client);
				/* (the player joined to multiplayer first, as a player picking
				their profile: the pregame screen then asks for them every
				frame until they are in the settings) */
				player_ui_local_player_joined_multiplayer_game(0);
				platform_log("network test: searching for games");
			}
		}
		else if (!network_test.joined && network_game_client_join_first_available_game())
		{
			network_test.joined = TRUE;
			/* (again, as the menus' joins do just before they join: a client the
			invite's screens made after the main menu cleared the players has none) */
			player_ui_local_player_joined_multiplayer_game(0);
			ui_widgets_close_all();
			ui_widget_load_by_name_or_tag(
				pc_menus_screen("ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen"),
				NONE, NULL, NONE, NONE, NONE, NONE);
			platform_log("network test: joining");
		}
		else if (network_test.joined && !network_test.player_added)
		{
			network_test.joined_seconds += seconds;
			if (network_test.joined_seconds >= 3.0f && global_network_game_client_get())
				network_test.player_added = TRUE;
		}
		/* (the other team from the host's player: a team game needs both) */
		else if (network_test.player_added && !network_test.team_set)
		{
			network_test.joined_seconds += seconds;
			if (network_test.joined_seconds >= 5.0f)
				network_test.team_set = network_game_client_set_team(NONE);
		}
		break;
	}
}
