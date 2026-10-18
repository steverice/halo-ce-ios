/* The provisional four-player benchmark probe's game-state guard. */
#ifndef MP_BENCHMARK_CONTEXT_PROBE
#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "main/main.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#endif
#include "mp_benchmark_probe.h"

#include <string.h>

int config_boolean(char const *name);
char const *config_string(char const *name);
short players_port_local_player_count(void);
int input_replay_suite_suspended(void);
static int suite_requested;

void mp_benchmark_probe_set_suite_requested(int requested)
{
	suite_requested = requested != 0;
}

int mp_benchmark_probe_requested(void)
{
	return suite_requested || (config_boolean("debug.multiplayer_benchmark_probe") &&
		!strcmp(config_string("debug.network_test"), "host:chillout") &&
		!config_string("debug.benchmark_suite")[0]);
}

int mp_benchmark_probe_active(void)
{
	struct scenario *scenario;
	struct game_variant *variant;

	if (!mp_benchmark_probe_requested())
		return 0;
	/* game_in_progress asserts that game time exists; early controller polls
	can arrive before game_initialize has made it. */
	if (!game_time_initialized() || !game_in_progress())
		return 0;
	scenario = global_scenario_try_and_get();
	if (!scenario || scenario->type != _scenario_type_multiplayer)
		return 0;
	if (game_connection() != _game_connection_network_server || !game_engine_running() ||
		!game_engine_can_score() || game_engine_showing_postgame() || game_engine_force_single_screen())
		return 0;
	variant = game_engine_get_variant();
	return mp_benchmark_probe_enabled(
		1,
		1,
		variant && variant->game_engine_index == game_engine_slayer &&
		mp_benchmark_probe_map_matches(main_get_multiplayer_map_name()),
		players_port_local_player_count(),
		suite_requested ? input_replay_suite_suspended() :
			(!config_string("debug.input_record")[0] && !config_string("debug.input_replay")[0]));
}

enum mp_benchmark_probe_input_policy mp_benchmark_probe_current_policy(void)
{
	int requested = mp_benchmark_probe_requested();

	return mp_benchmark_probe_policy(requested, requested && mp_benchmark_probe_active());
}

long mp_benchmark_probe_tick(void)
{
	return mp_benchmark_probe_active() ? game_time_get() : -1;
}
