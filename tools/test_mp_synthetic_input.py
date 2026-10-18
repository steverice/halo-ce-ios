#!/usr/bin/env python3
"""Run the host's real distributed-input queue and tick snapshot with suite actions."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest


ROOT = Path(__file__).resolve().parents[1]
QUEUES = (ROOT / "source/game/player_queues_new.c").read_text()
NETWORK_TEST = (ROOT / "port/linux/game/network_test.c").read_text()
UNITS = (ROOT / "source/units/units.c").read_text()
UNIT_HEADER = (ROOT / "source/units/units.h").read_text()
WEAPON_HEADER = (ROOT / "source/items/weapons.h").read_text()


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise ValueError(f"Unclosed source function: {signature}")


HARNESS = r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int boolean;
typedef float real;
typedef unsigned short word;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define FLAG(bit) (1UL << (bit))
#define TEST_FLAG(flags, bit) (((flags) & FLAG(bit)) != 0)
#define SET_FLAG(flags, bit, value) do { if (value) (flags) |= FLAG(bit); else (flags) &= ~FLAG(bit); } while (0)
__UNIT_CONTROL_ENUM__
__WEAPON_CONTROL_ENUM__
#define MAXIMUM_NUMBER_OF_PLAYERS 128
#define MAXIMUM_LOCAL_PLAYERS 4
#define MAXIMUM_WEAPONS_PER_UNIT 4
#define NUMBER_OF_UNIT_GRENADE_TYPES 2
#define DISTRIBUTED_INPUT_HISTORY 4
#define DISTRIBUTED_INPUT_SILENCE_TICKS 10
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(index) ((int)((index) & 0xffff))
#define csmemset memset
#define csmemcpy memcpy
#define match_assert(file, line, expression) do { (void)(file); (void)(line); if (!(expression)) abort(); } while (0)
enum { _game_connection_local, _game_connection_network_server };
struct real_vector2d { float i, j; };
struct real_euler_angles2d { float yaw, pitch; };
struct player_action {
    unsigned long control_flags;
    struct real_euler_angles2d desired_facing;
    struct real_vector2d throttle;
    float primary_trigger;
    short desired_weapon_index, desired_grenade_index, desired_zoom_level, pad;
};
struct server_update { word action_count; short pad; struct player_action actions[MAXIMUM_NUMBER_OF_PLAYERS]; };
struct update { long update_number; struct server_update update; };
struct update_server_queue_datum { short identifier, pad; long next_update_number; struct player_action current_action; };
struct data_array { int count; struct update_server_queue_datum *data; };
static struct update_server_queue_datum queues[2];
static struct data_array queue_array = { 2, queues };
static struct { int initialized; long next_update_number_to_build; struct data_array *queues; } update_server_globals = {
    TRUE, 0, &queue_array
};
static struct { boolean valid; long tick, received_time; } update_server_distributed_inputs[MAXIMUM_NUMBER_OF_PLAYERS];
static unsigned long update_server_pending_control_flags[MAXIMUM_NUMBER_OF_PLAYERS];
static struct update current_update;
static struct server_update observed;
static long game_tick, machine_players[MAXIMUM_LOCAL_PLAYERS] = { NONE, NONE, NONE, NONE };
static struct { int suite_owned, players_wanted; } network_test;
static int game_connection(void) { return _game_connection_network_server; }
static boolean game_time_initialized(void) { return TRUE; }
static long game_time_get(void) { return game_tick; }
static int input_replay_sim_isolated(void) { return FALSE; }
static long player_control_take_frame_action_flags(void) { return 0; }
static void update_server_take_local_actions(void) { }
static struct update *update_server_get_update(long number) { (void)number; return &current_update; }
static void update_client_handle_server_update(const struct server_update *update, long number) {
    (void)number; observed = *update;
}
static void *datum_try_and_get(struct data_array *array, long player_index) {
    int absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
    if (absolute < 0 || absolute >= array->count ||
        array->data[absolute].identifier != (short)(player_index >> 16)) return NULL;
    return &array->data[absolute];
}
static long *machine_get_player_list(long machine_index) {
    return machine_index == 1 ? machine_players : NULL;
}
static int valid_real(float value) { return isfinite(value); }
void network_test_suite_synthetic_input_tick(long tick);
__ACTION_VALID__
__ADD_TICKS__
__HANDLE_INPUT__
__SYNTHETIC_INPUT__
__NEXT_UPDATE__
static long actual_unit_primary_weapon_flags(unsigned long control_flags) {
    struct { struct { unsigned long control_flags; } unit; } body, *unit = &body;
    unit->unit.control_flags = control_flags;
    long flags = 0;
    __UNIT_PRIMARY_GATE__
    return flags;
}
static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "synthetic input line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
int main(void) {
    machine_players[0] = (1L << 16) | 1;
    queues[0].identifier = queues[1].identifier = 1;
    network_test.suite_owned = network_test.players_wanted = 1;
    game_tick = 10;
    update_server_next_update();
    CHECK(observed.action_count == 2 && observed.actions[1].throttle.j > 0.5f &&
        observed.actions[1].primary_trigger > 0.5f);
    CHECK(TEST_FLAG(observed.actions[1].control_flags, _unit_control_weapon_primary_trigger_bit));
    CHECK(TEST_FLAG(actual_unit_primary_weapon_flags(observed.actions[1].control_flags), _weapon_control_primary_trigger_bit));
    CHECK(update_server_distributed_inputs[1].valid && update_server_distributed_inputs[1].tick == 10);
    {
        struct player_action duplicate = queues[1].current_action;
        word flags[DISTRIBUTED_INPUT_HISTORY] = { 0 };
        duplicate.throttle.j = -1.0f;
        update_server_handle_distributed_input(machine_players[0], 10, &duplicate, flags, DISTRIBUTED_INPUT_HISTORY);
        CHECK(queues[1].current_action.throttle.j > 0.5f);
    }
    game_tick = 60; /* release phase must clear both the analog pull and digital fire flag */
    update_server_next_update();
    CHECK(observed.actions[1].primary_trigger == 0.0f);
    CHECK(!TEST_FLAG(observed.actions[1].control_flags, _unit_control_weapon_primary_trigger_bit));
    CHECK(!actual_unit_primary_weapon_flags(observed.actions[1].control_flags));
    network_test.suite_owned = 0;
    memset(&queues[1].current_action, 0, sizeof(queues[1].current_action));
    game_tick = 61;
    update_server_next_update();
    CHECK(observed.actions[1].throttle.j == 0.0f && observed.actions[1].primary_trigger == 0.0f);
    network_test.suite_owned = 1;
    queues[1].identifier = 0; /* missing datum queue must stay inert */
    game_tick = 62;
    update_server_next_update();
    CHECK(observed.actions[1].throttle.j == 0.0f && observed.actions[1].primary_trigger == 0.0f);
    return failures != 0;
}
'''


def test_suite_input_reaches_host_tick_snapshot() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise SkipTest("A C compiler is required for the synthetic input probe")
    synthetic = ""
    if "void network_test_suite_synthetic_input_tick(" in NETWORK_TEST:
        synthetic = function(NETWORK_TEST, "void network_test_suite_synthetic_input_tick(")
    source = HARNESS
    def control_enum(header, first):
        name = header.index(first)
        start = header.rfind("enum", 0, name)
        end = header.index("};", name) + 2
        return header[start:end]

    for key, value in {
        "__UNIT_CONTROL_ENUM__": control_enum(UNIT_HEADER, "_unit_control_crouch_modifier_bit"),
        "__WEAPON_CONTROL_ENUM__": control_enum(WEAPON_HEADER, "_weapon_control_integrated_light_bit"),
        "__UNIT_PRIMARY_GATE__": function(UNITS, "if (TEST_FLAG(unit->unit.control_flags, _unit_control_weapon_primary_trigger_bit))"),
        "__ACTION_VALID__": function(QUEUES, "static boolean distributed_action_valid("),
        "__ADD_TICKS__": function(QUEUES, "static boolean distributed_add_new_ticks("),
        "__HANDLE_INPUT__": function(QUEUES, "void update_server_handle_distributed_input("),
        "__SYNTHETIC_INPUT__": synthetic,
        "__NEXT_UPDATE__": function(QUEUES, "void update_server_next_update("),
    }.items():
        source = source.replace(key, value)
    with tempfile.TemporaryDirectory(prefix="mp-synthetic-input-") as temporary:
        path = Path(temporary) / "input.c"
        binary = Path(temporary) / "input"
        path.write_text(source)
        subprocess.run([compiler, "-O2", "-Wall", str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    test_suite_input_reaches_host_tick_snapshot()
