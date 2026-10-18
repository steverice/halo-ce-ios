/* The provisional four-player benchmark probe's game-state guard. */
#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "main/main.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "mp_benchmark_probe.h"

#include <string.h>

int config_boolean(char const *name);
char const *config_string(char const *name);
short players_port_local_player_count(void);

int mp_benchmark_probe_active(void)
{
	static int requested = -1;
	struct scenario *scenario;
	struct game_variant *variant;

	if (requested < 0)
		requested = config_boolean("debug.multiplayer_benchmark_probe");
	if (!requested ||
		strcmp(config_string("debug.network_test"), "host:chillout") ||
		config_string("debug.benchmark_suite")[0])
		return 0;
	scenario = global_scenario_try_and_get();
	variant = game_engine_running() ? game_engine_get_variant() : NULL;
	return mp_benchmark_probe_enabled(
		1,
		game_connection() == _game_connection_network_server,
		scenario && scenario->type == _scenario_type_multiplayer && variant &&
		variant->game_engine_index == game_engine_slayer &&
		mp_benchmark_probe_map_matches(main_get_multiplayer_map_name()),
		players_port_local_player_count(),
		!config_string("debug.input_record")[0] && !config_string("debug.input_replay")[0]);
}

long mp_benchmark_probe_tick(void)
{
	return mp_benchmark_probe_active() ? game_time_get() : -1;
}
