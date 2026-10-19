#!/usr/bin/env python3
"""Observe real local and synthetic player datums through the production benchmark adapter."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "port/linux/game/benchmark_scenario.c").read_text()


def function(signature: str) -> str:
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth = 0
    for position in range(opening, len(SOURCE)):
        if SOURCE[position] == "{":
            depth += 1
        elif SOURCE[position] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:position + 1]
    raise ValueError(f"Unclosed benchmark state function: {signature}")


HARNESS = r'''
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HALO_PORT_MAXIMUM_NETWORK_PLAYERS 128
#include "benchmark_mp_state.h"
#define NONE -1
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(index) (index)
#define _scenario_type_multiplayer 2
#define _game_connection_network_server 2
#define game_engine_slayer 2
#define _object_mask_projectile 8
typedef struct { float x, y, z; } real_point3d;
struct scenario { int type; };
struct game_variant { int game_engine_index; struct { int score_to_win; } universal_variant; };
struct game_variant_options { int time_limit; };
struct player_datum {
    int unit_index, local_player_index;
    struct { int kills[4], deaths; } statistics;
    struct { int machine_index; } network_player_data;
};
struct object_datum { struct { float body_vitality, shield_vitality; } object; };
struct unit_datum {
    struct object_datum object_datum;
    real_point3d origin;
    struct { struct { float i, j; } throttle; int grenade_counts[2];
        int current_weapon_index; long weapon_object_indices[4]; } unit;
};
struct weapon_datum { int definition_index; struct { struct { int rounds_total, rounds_loaded; } magazines[1]; } weapon; };
struct mp_benchmark_probe_counts { unsigned long fire, jump, grenade; };
struct object_iterator { int index; };
static struct scenario scenario = { _scenario_type_multiplayer };
static struct game_variant variant = { game_engine_slayer, { 1000 } };
static struct game_variant_options options;
static struct player_datum players[129];
static struct unit_datum units[129];
static struct weapon_datum weapons[129];
static struct player_action { unsigned long control_flags; struct {float yaw, pitch;} desired_facing;
    struct {float i, j;} throttle; float primary_trigger; short weapon, grenade, zoom, pad; } actions[129];
static int remote_present = 1, queue_ready = 1, projectile_present = 1;
static int expected_players = 2, actual_players = 2;
static uint32_t network_test_suite_energy_projectiles(int actor) {return (uint32_t)(actor+7);}
struct data_iterator { int index; long datum_index; };
static void *player_data;
static struct player_datum *player_try_and_get(long index);
static void data_iterator_new(struct data_iterator *iterator, void *unused) {
    (void)unused; iterator->index = 0;
}
static void *data_iterator_next(struct data_iterator *iterator) {
    while (iterator->index < actual_players) {
        iterator->datum_index = iterator->index++;
        struct player_datum *player = player_try_and_get(iterator->datum_index);
        if (player) return player;
    }
    return NULL;
}
static int game_time_initialized(void) { return 1; }
static int game_in_progress(void) { return 1; }
static struct scenario *global_scenario_try_and_get(void) { return &scenario; }
static int game_connection(void) { return _game_connection_network_server; }
static long game_time_get(void) { return 90; }
static char *main_get_multiplayer_map_name(void) { return "levels\\test\\chillout\\chillout"; }
static int mp_benchmark_probe_map_matches(const char *map) { return strstr(map, "chillout") != NULL; }
static struct game_variant *game_engine_get_variant(void) { return &variant; }
static struct game_variant_options *game_variant_options_get(void) { return &options; }
static short players_port_local_player_count(void) { return 1; }
static short main_get_window_count(void) { return 1; }
static int game_engine_can_score(void) { return 1; }
static int game_engine_showing_postgame(void) { return 0; }
static int benchmark_mp_arena_ready(void) { return 1; }
static unsigned long network_test_suite_repositions(int actor) { return actor >= 0 && actor < 33 ? 1 : 0; }
static void network_test_suite_scripted_counts(unsigned long *hits,unsigned long *kills) { *hits = 3; *kills = 1; }
static int network_test_suite_network_players(void) { return expected_players; }
static int network_test_suite_map_matches(const char *map) { return strstr(map, "chillout") != NULL; }
static long local_player_get_player_index(short local) { return local == 0 ? 0 : NONE; }
static long *machine_get_player_list(long machine) {
    static long lists[129][4];
    if (machine < 0 || machine >= expected_players) return NULL;
    lists[machine][0] = machine;
    lists[machine][1] = lists[machine][2] = lists[machine][3] = NONE;
    return lists[machine];
}
static struct player_datum *player_try_and_get(long index) {
    return index >= 0 && index < actual_players && (index != 1 || remote_present) ? &players[index] : NULL;
}
static void object_get_origin(long index, real_point3d *origin) { *origin = units[index].origin; }
static struct object_datum *object_get(long index) { return &units[index].object_datum; }
static struct unit_datum *unit_get(long index) { return &units[index]; }
static struct weapon_datum *weapon_get(long index) { return &weapons[index]; }
static void *observer_get_camera(short local) { return local == 0 ? &scenario : NULL; }
static int mp_benchmark_probe_active(void) { return 1; }
static void mp_benchmark_probe_action_counts(struct mp_benchmark_probe_counts counts[4]) {
    memset(counts, 0, sizeof(*counts) * 4); counts[0].fire = 3;
}
static long update_server_ticked_update_number(void) { return queue_ready ? 90 : NONE; }
static const struct player_action *update_server_update_actions(long tick, short *count) {
    if (tick == NONE) return NULL;
    *count = expected_players; return actions;
}
static void object_iterator_new(struct object_iterator *iterator, int mask, int unused) {
    (void)mask; (void)unused; iterator->index = 0;
}
static struct object_datum *object_iterator_next(struct object_iterator *iterator) {
    return projectile_present && iterator->index++ == 0 ? &units[0].object_datum : NULL;
}
__ACTOR_HELPER__
__STATE_FUNCTION__
static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "MP state line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
int main(void) {
    for (int index = 0; index < 129; index++) {
        players[index].unit_index = index;
        players[index].local_player_index = index ? NONE : 0;
        players[index].network_player_data.machine_index = index;
        units[index].origin.x = (float)(index * 12);
        units[index].object_datum.object.body_vitality = 1.0f;
        units[index].object_datum.object.shield_vitality = 1.0f;
        units[index].unit.throttle.j = 0.75f;
        units[index].unit.grenade_counts[0] = 1;
        units[index].unit.current_weapon_index = 0;
        units[index].unit.weapon_object_indices[0] = index;
        weapons[index].definition_index = 1;
        weapons[index].weapon.magazines[0].rounds_loaded = 30;
    }
    actions[1].throttle.j = 0.75f; actions[1].primary_trigger = 1.0f;
    struct benchmark_mp_state state;
    CHECK(sizeof(state) < 16 * 1024);
    fprintf(stderr, "benchmark_mp_state_size=%lu bytes actor_limit=%d\n",
        (unsigned long)sizeof(state), BENCHMARK_MP_ACTORS);
    benchmark_scenario_multiplayer_state(&state);
    CHECK(state.local_players == 1 && state.roster == 2 && state.spawned == 2);
    CHECK(state.actors[1].present && state.actors[1].spawned && state.actors[1].position[0] == 12.0f);
    CHECK(state.synthetic_queue_ready && state.projectiles_alive == 1);
    CHECK(state.actors[1].repositions == 1 && state.scripted_hits == 3 && state.scripted_kills == 1);
    CHECK(state.actors[1].energy_projectiles == 8 && sizeof(state.actors[1].energy_projectiles)==4);
    players[1].unit_index=NONE;
    benchmark_scenario_multiplayer_state(&state);
    CHECK(!state.actors[1].spawned&&state.actors[1].energy_projectiles==8);
    players[1].unit_index=1;
    remote_present = 0; queue_ready = 0; projectile_present = 0;
    benchmark_scenario_multiplayer_state(&state);
    CHECK(state.roster == 1 && state.spawned == 1 && !state.synthetic_queue_ready && !state.projectiles_alive);
    remote_present = queue_ready = projectile_present = 1;
    for (int count = 16; count <= 128; count *= 2) {
        expected_players = actual_players = count;
        for (int i = 1; i < count; i++) actions[i].throttle.j = 0.75f;
        benchmark_scenario_multiplayer_state(&state);
        CHECK(BENCHMARK_MP_ACTORS == 128);
        CHECK(state.roster == count && state.spawned == count && state.synthetic_queue_ready);
        for (int i = 0; i < count && i < BENCHMARK_MP_ACTORS; i++) CHECK(state.actors[i].present && state.actors[i].spawned);
        actual_players = count + 1;
        benchmark_scenario_multiplayer_state(&state);
        CHECK(state.roster == count + 1 && state.spawned == count + 1);
        actual_players = count;
        remote_present = 0;
        benchmark_scenario_multiplayer_state(&state);
        CHECK(state.roster == count - 1 && state.spawned == count - 1 && !state.synthetic_queue_ready);
        remote_present = 1;
        actions[count - 1].throttle.j = 0.0f;
        benchmark_scenario_multiplayer_state(&state);
        CHECK(!state.synthetic_queue_ready);
        actions[count - 1].throttle.j = 0.75f;
    }
    expected_players = actual_players = 129;
    benchmark_scenario_multiplayer_state(&state);
    CHECK(state.roster == 129 && state.spawned == 129 && !state.synthetic_queue_ready);
    return failures != 0;
}
'''


def test_real_synthetic_datum_queue_and_projectile_observation() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise SkipTest("A C compiler is required for the MP state probe")
    helper = ""
    if "static void benchmark_mp_actor_fill(" in SOURCE:
        helper = function("static void benchmark_mp_actor_fill(")
    source = HARNESS.replace("__ACTOR_HELPER__", helper).replace(
        "__STATE_FUNCTION__", function("void benchmark_scenario_multiplayer_state("))
    with tempfile.TemporaryDirectory(prefix="mp-benchmark-state-") as temporary:
        path = Path(temporary) / "state.c"
        binary = Path(temporary) / "state"
        path.write_text(source)
        subprocess.run([compiler, "-O2", "-Wall", "-iquote", str(ROOT / "port/linux/include"),
                        str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    test_real_synthetic_datum_queue_and_projectile_observation()
