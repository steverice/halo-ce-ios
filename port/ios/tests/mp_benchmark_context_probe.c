/* Exercise the actual game-state adapter without starting or building a game. */
#include <stdio.h>
#include <string.h>

enum { _scenario_type_main_menu = 0, _scenario_type_multiplayer = 2 };
enum { _game_connection_local = 0, _game_connection_network_server = 2 };
enum { game_engine_slayer = 2 };
struct scenario { int type; };
struct game_variant { int game_engine_index; };

static int requested, initialized, in_progress, scoring, forced_single, postgame;
static int local_count, connection, getter_calls, tick_calls;
static int suite_suspended;
static const char *network_test, *suite, *record, *replay, *map;
static struct scenario scenario;
static struct game_variant variant;

int config_boolean(const char *name) { return !strcmp(name, "debug.multiplayer_benchmark_probe") ? requested : 0; }
const char *config_string(const char *name)
{
	if (!strcmp(name, "debug.network_test")) return network_test;
	if (!strcmp(name, "debug.benchmark_suite")) return suite;
	if (!strcmp(name, "debug.input_record")) return record;
	if (!strcmp(name, "debug.input_replay")) return replay;
	return "";
}
int game_time_initialized(void) { return initialized; }
int game_in_progress(void) { getter_calls++; return in_progress; }
struct scenario *global_scenario_try_and_get(void) { getter_calls++; return &scenario; }
int game_connection(void) { getter_calls++; return connection; }
int game_engine_running(void) { getter_calls++; return 1; }
int game_engine_can_score(void) { getter_calls++; return scoring; }
int game_engine_force_single_screen(void) { getter_calls++; return forced_single; }
int game_engine_showing_postgame(void) { getter_calls++; return postgame; }
struct game_variant *game_engine_get_variant(void) { getter_calls++; return &variant; }
char *main_get_multiplayer_map_name(void) { getter_calls++; return (char *)map; }
short players_port_local_player_count(void) { getter_calls++; return (short)local_count; }
long game_time_get(void) { tick_calls++; return 123; }
int input_replay_suite_suspended(void) { return suite_suspended; }

#define MP_BENCHMARK_CONTEXT_PROBE 1
#include "../../linux/game/mp_benchmark_context.c"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
	fprintf(stderr, "mp_benchmark_context_probe: line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static void reset(void)
{
	requested = initialized = in_progress = scoring = 1;
	mp_benchmark_probe_set_suite_requested(0);
	suite_suspended = 0;
	forced_single = postgame = 0;
	local_count = 4;
	connection = _game_connection_network_server;
	getter_calls = tick_calls = 0;
	network_test = "host:chillout";
	suite = record = replay = "";
	map = "levels\\test\\chillout\\chillout";
	scenario.type = _scenario_type_multiplayer;
	variant.game_engine_index = game_engine_slayer;
}

int main(void)
{
	reset(); requested = 0;
	CHECK(!mp_benchmark_probe_requested() && getter_calls == 0);
	CHECK(mp_benchmark_probe_current_policy() == mp_benchmark_probe_ordinary_input && getter_calls == 0);
	CHECK(!mp_benchmark_probe_active() && getter_calls == 0);
	reset(); initialized = 0;
	CHECK(mp_benchmark_probe_requested() && getter_calls == 0);
	CHECK(mp_benchmark_probe_current_policy() == mp_benchmark_probe_neutral_input && getter_calls == 0);
	CHECK(!mp_benchmark_probe_active() && getter_calls == 0);
	CHECK(mp_benchmark_probe_tick() == -1 && tick_calls == 0);
	reset(); in_progress = 0;
	CHECK(!mp_benchmark_probe_active() && getter_calls == 1);
	reset(); scenario.type = _scenario_type_main_menu;
	CHECK(!mp_benchmark_probe_active() && getter_calls == 2);
	CHECK(mp_benchmark_probe_current_policy() == mp_benchmark_probe_neutral_input);
	reset(); scoring = 0; postgame = forced_single = 1;
	CHECK(mp_benchmark_probe_requested());
	CHECK(mp_benchmark_probe_current_policy() == mp_benchmark_probe_neutral_input);
	CHECK(!mp_benchmark_probe_active());
	CHECK(mp_benchmark_probe_tick() == -1 && tick_calls == 0);
	reset(); forced_single = 1;
	CHECK(!mp_benchmark_probe_active());
	reset(); postgame = 1;
	CHECK(!mp_benchmark_probe_active());
	reset(); record = "auto";
	CHECK(!mp_benchmark_probe_active());
	CHECK(mp_benchmark_probe_current_policy() == mp_benchmark_probe_neutral_input);
	reset(); replay = "route.input";
	CHECK(!mp_benchmark_probe_active());
	CHECK(mp_benchmark_probe_current_policy() == mp_benchmark_probe_neutral_input);
	reset();
	CHECK(mp_benchmark_probe_active());
	CHECK(mp_benchmark_probe_current_policy() == mp_benchmark_probe_scripted_input);
	CHECK(mp_benchmark_probe_tick() == 123 && tick_calls == 1);
	reset(); requested = 0; network_test = ""; suite = "full"; record = "auto";
	mp_benchmark_probe_set_suite_requested(1);
	CHECK(mp_benchmark_probe_requested());
	CHECK(!mp_benchmark_probe_active());
	CHECK(mp_benchmark_probe_current_policy() == mp_benchmark_probe_neutral_input);
	suite_suspended = 1;
	CHECK(mp_benchmark_probe_active());
	CHECK(mp_benchmark_probe_current_policy() == mp_benchmark_probe_scripted_input);
	mp_benchmark_probe_set_suite_requested(0);
	CHECK(!mp_benchmark_probe_requested());
	CHECK(mp_benchmark_probe_current_policy() == mp_benchmark_probe_ordinary_input);
	if (failures) fprintf(stderr, "mp_benchmark_context_probe: %d failures\n", failures);
	return failures ? 1 : 0;
}
