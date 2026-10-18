#!/usr/bin/env python3
"""Run the real lobby map-set branch and player-table functions with one synthetic machine."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest


ROOT = Path(__file__).resolve().parents[1]
NETWORK_TEST = (ROOT / "port/linux/game/network_test.c").read_text()
GAME_MANAGER = (ROOT / "source/networking/network_game_manager.c").read_text()
GAME_MESSAGES = (ROOT / "source/networking/network_server_message_handler.c").read_text()
GAME_SERVER = (ROOT / "source/networking/network_server_manager.c").read_text()


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


def lobby_branch() -> str:
    start = NETWORK_TEST.index("if (!network_test.map_set && network_test.setup_seconds >= 1.0f")
    end = NETWORK_TEST.index("\n\t\t\tif (!network_test.player_added", start)
    return NETWORK_TEST[start:end]


def after_map_branch() -> str:
    start = NETWORK_TEST.index("\n\t\t\tif (!network_test.player_added", NETWORK_TEST.index("case _network_test_host:"))
    end = NETWORK_TEST.index("\n\t\t}\n\t\tbreak;\n\tcase _network_test_join:", start)
    return NETWORK_TEST[start:end]


def suite_functions() -> str:
    pieces = [function(NETWORK_TEST, "int network_test_suite_begin("),
              function(NETWORK_TEST, "void network_test_suite_end(void)")]
    if "static int network_test_suite_add_synthetic_players(" in NETWORK_TEST:
        pieces.append(function(NETWORK_TEST, "static int network_test_suite_add_synthetic_players("))
    if "static int network_test_suite_prepare_synthetic_players(" in NETWORK_TEST:
        pieces.append(function(NETWORK_TEST, "static int network_test_suite_prepare_synthetic_players("))
    return "\n\n".join(pieces)


HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef int boolean;
typedef float real;
typedef unsigned char byte;
typedef unsigned short word;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define MAXIMUM_NETWORK_MACHINE_COUNT 128
#define MAXIMUM_LOCAL_PLAYERS 4
#define NETWORK_GAME_MACHINE_SLOTS 128
#define NETWORK_GAME_PLAYER_SLOTS 128
#define NETWORK_SERVER_MANAGER_FILE "network_server_manager.c"
#define VALID_INDEX(index, maximum) ((index) >= 0 && (index) < (maximum))
#define TEST_FLAG(flags, bit) (((flags) & (1UL << (bit))) != 0)
#define csmemcpy memcpy
#define match_assert(file, line, expression) do { (void)(file); (void)(line); if (!(expression)) abort(); } while (0)
#define network_machine_is_valid(machine) ((machine) && (machine)->machine_index >= 0 && \
    (machine)->machine_index < NETWORK_GAME_MACHINE_SLOTS)
enum { _network_test_off, _network_test_host, _network_test_join, _network_test_browse };
enum { _game_connection_local, _game_connection_network_server };
enum { _game_difficulty_level_normal = 1 };
enum { _network_client_machine_validated_bit = 0 };
struct network_machine { wchar_t name[32]; char machine_index; byte padding[3]; };
struct network_player {
    wchar_t name[12]; short primary_color_index, icon_index;
    char machine_index, controller_index, team_index, player_list_index;
};
struct game_variant { struct { short score_to_win; } universal_variant; };
struct game_variant_options { short time_limit; };
struct network_game {
    struct game_variant_options variant_options;
    short machine_count, player_count;
    byte maximum_players;
    struct network_machine machines[NETWORK_GAME_MACHINE_SLOTS];
    struct network_player players[NETWORK_GAME_PLAYER_SLOTS];
    char map_name[128];
};
struct network_game_server_client_machine { unsigned long flags; };
struct network_game_server {
    struct network_game game;
    struct network_game_server_client_machine client_machines[MAXIMUM_NETWORK_MACHINE_COUNT];
    int joined_transport_machines;
};
static struct network_game_server server;
static int settings_sends, sent_roster_count, local_requests, start_requests;
static unsigned long now_ms, network_game_settings_update_time;
static boolean network_game_settings_update_pending;
enum { NETWORK_GAME_SETTINGS_UPDATE_INTERVAL = 250 };
static int force_slots_held, force_settings_failure;
static int network_game_player_slot_held(long index) { return force_slots_held == 1 || (force_slots_held < 0 && index >= -force_slots_held); }
static void network_event(const char *format, ...) { (void)format; }
static void error(int kind, const char *format, ...) { (void)kind; (void)format; }
__PLAYER_VALID__
__ADD_MACHINE__
__ADD_PLAYER__
__INVALIDATE_PLAYER__
__INVALIDATE_MACHINE__
__PLAYER_IS_VALID__
__REMOVE_PLAYER__
__REMOVE_MACHINE__
__CLIENT_JOINED__
__ENOUGH_MACHINES__
static const char *config_string(const char *key) { (void)key; return ""; }
static int game_connection(void) { return _game_connection_local; }
static void network_game_abort(void) { }
static struct network_game_server *global_network_game_server_get(void) { return &server; }
static struct network_game *network_game_get_game(void) { return &server.game; }
static struct network_game *network_game_server_get_game(struct network_game_server *host) { return &host->game; }
static unsigned long system_milliseconds(void) { return now_ms; }
static int network_game_server_send_game_settings_to_all_machines(struct network_game_server *host,
    const void *game, long size) {
    (void)host; (void)size;
    if (force_settings_failure) return FALSE;
    settings_sends++; sent_roster_count = ((const struct network_game *)game)->player_count;
    network_game_settings_update_pending = FALSE;
    network_game_settings_update_time = now_ms;
    return TRUE;
}
__FLUSH_SETTINGS__
__SEND_SETTINGS__
static void network_game_server_change_map_name(struct network_game_server *host, const char *path) {
    snprintf(host->game.map_name, sizeof(host->game.map_name), "%s", path);
    network_game_server_send_game_data_pregame(host);
}
static struct game_variant *game_engine_get_variant_by_name(struct game_variant *out, const char *name) {
    (void)name; memset(out, 0, sizeof(*out)); return out;
}
static void player_ui_set_game_variant(struct game_variant *variant) { (void)variant; }
static void network_game_server_change_game_variant(struct network_game_server *host, struct game_variant *variant) {
    (void)variant; network_game_server_send_game_data_pregame(host);
}
static void platform_log(const char *format, ...) { (void)format; }
static void *global_network_game_client_get(void) { return &server; }
static int network_game_client_add_player(void *client, int controller) {
    (void)client; (void)controller; local_requests++; return TRUE;
}
static void network_game_client_request_immediate_start(void) { start_requests++; }
static void network_game_server_pause_countdown(struct network_game_server *host, int pause) {
    (void)host; (void)pause;
}
static int ui_widget_port_cooperative_level_choose(const char *path, short difficulty) {
    (void)path; (void)difficulty; return FALSE;
}
static void reset_game(void) {
    int index;
    memset(&server, 0, sizeof(server));
    for (index = 0; index < NETWORK_GAME_MACHINE_SLOTS; index++) server.game.machines[index].machine_index = NONE;
    for (index = 0; index < NETWORK_GAME_PLAYER_SLOTS; index++) {
        server.game.players[index].machine_index = NONE;
        server.game.players[index].controller_index = NONE;
        server.game.players[index].player_list_index = NONE;
    }
    server.game.maximum_players = 128;
    server.game.machine_count = server.game.player_count = server.joined_transport_machines = 1;
    server.client_machines[0].flags = 1UL << _network_client_machine_validated_bit;
    server.game.machines[0].machine_index = 0;
    server.game.players[0].machine_index = 0;
    server.game.players[0].controller_index = 0;
    server.game.players[0].player_list_index = 0;
    settings_sends = sent_roster_count = local_requests = start_requests = 0;
    force_slots_held = force_settings_failure = 0;
    now_ms = network_game_settings_update_time = 0;
    network_game_settings_update_pending = FALSE;
}
static void benchmark_mp_arena_reset(void) { }
__NETWORK_TEST_STATE__
__SUITE_FUNCTIONS__
__VARIANT_FUNCTION__
static void actual_lobby_map_set(void) {
    __MAP_SET_BRANCH__
}
static void actual_lobby_after_map(void) {
    if (network_test.mode != _network_test_host) return;
    __AFTER_MAP_BRANCH__
}
static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "synthetic roster line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
int main(void) {
    /* Actual startup joins the machine before its local player request is sent. */
    reset_game();
    network_game_invalidate_player(&server.game.players[0]);
    server.game.player_count = 0;
    CHECK(network_test_suite_begin("network_players", "chillout", "slayer", 2, 1, 37, 7));
    network_test.setup_seconds = 1.0f;
    actual_lobby_map_set();
    CHECK(network_test.mode == _network_test_host && network_test.map_set);
    CHECK(server.game.machine_count == 1 && server.game.player_count == 0);
    network_test.setup_seconds = 2.0f;
    actual_lobby_after_map();
    CHECK(local_requests == 1 && network_test.player_added);
    CHECK(network_test.mode == _network_test_host && server.game.player_count == 0 && start_requests == 0);
    {
        struct network_player local = { 0 };
        local.machine_index = 0; local.controller_index = 0;
        local.team_index = NONE; local.player_list_index = NONE;
        CHECK(network_game_add_player(&server.game, &local));
    }
    network_test.setup_seconds = 3.0f;
    actual_lobby_after_map();
    CHECK(network_test.mode == _network_test_host && server.game.player_count == 2 && server.game.machine_count == 2);
    actual_lobby_after_map();
    CHECK(server.game.player_count == 2 && server.game.machine_count == 2 && start_requests == 0);
    network_test.setup_seconds = 15.0f;
    actual_lobby_after_map();
    CHECK(network_test.started && start_requests == 1);
    network_test_suite_end();

    reset_game();
    network_game_invalidate_player(&server.game.players[0]); server.game.player_count = 0;
    CHECK(network_test_suite_begin("network_players", "chillout", "slayer", 2, 1, 37, 7));
    network_test.setup_seconds = 1.0f; actual_lobby_map_set();
    CHECK(network_test.mode == _network_test_host);
    network_test.setup_seconds = 2.0f; actual_lobby_after_map();
    CHECK(network_test.mode == _network_test_host && start_requests == 0);
    network_test.setup_seconds = 15.0f; actual_lobby_after_map();
    CHECK(network_test.mode == _network_test_off && !network_test.started && start_requests == 0);
    CHECK(server.game.player_count == 0 && server.game.machine_count == 1);
    network_test_suite_end();

    reset_game();
    CHECK(network_test_suite_begin("network_players", "chillout", "slayer", 2, 1, 37, 7));
    CHECK(network_test.players_wanted == 1); /* one real local controller */
    network_test.setup_seconds = 1.0f;
    actual_lobby_map_set();
    CHECK(network_test.map_set && server.joined_transport_machines == 1);
    CHECK(server.game.variant_options.time_limit == 7);
    network_test.setup_seconds = 2.0f; actual_lobby_after_map();
    CHECK(server.game.machine_count == 2 && server.game.player_count == 2);
    CHECK(server.game.machines[1].machine_index == 1);
    CHECK(server.game.players[1].machine_index == 1 && server.game.players[1].controller_index == 0);
    CHECK(server_has_enough_machines(&server) && !server.client_machines[1].flags);
    server.client_machines[0].flags = 0;
    CHECK(!server_has_enough_machines(&server));
    server.client_machines[0].flags = 1UL << _network_client_machine_validated_bit;
    CHECK(network_game_settings_update_pending && settings_sends == 0);
    now_ms = 250;
    CHECK(network_game_server_flush_game_data_pregame(&server));
    CHECK(settings_sends == 1 && sent_roster_count == 2 && !network_game_settings_update_pending);
    CHECK(!strcmp(server.game.map_name, "levels\\test\\chillout\\chillout"));
    CHECK(network_game_remove_machine(&server.game, &server.game.machines[1]));
    CHECK(server.game.machine_count == 1 && server.game.player_count == 1 &&
        server.game.machines[1].machine_index == NONE && server.game.players[1].machine_index == NONE);

    reset_game();
    memset(&network_test, 0, sizeof(network_test));
    network_test.mode = _network_test_host;
    snprintf(network_test.map_name, sizeof(network_test.map_name), "chillout");
    snprintf(network_test.variant_name, sizeof(network_test.variant_name), "slayer");
    network_test.players_wanted = 1;
    network_test.setup_seconds = 1.0f;
    actual_lobby_map_set();
    CHECK(server.game.machine_count == 1 && server.game.player_count == 1);
    now_ms = 250;
    CHECK(network_game_server_flush_game_data_pregame(&server));
    CHECK(settings_sends == 1 && sent_roster_count == 1);

    reset_game();
    memset(&network_test, 0, sizeof(network_test));
    CHECK(network_test_suite_begin("network_players", "chillout", "slayer", 2, 1, 37, 7));
    network_test.setup_seconds = 1.0f;
    force_slots_held = TRUE;
    actual_lobby_map_set();
    network_test.setup_seconds = 2.0f; actual_lobby_after_map();
    CHECK(network_test.mode == _network_test_off);
    CHECK(!network_test_suite_begin("network_players", "chillout", "slayer", 2, 1, 37, 7));
    CHECK(server.game.machine_count == 1 && server.game.player_count == 1 &&
        server.game.machines[1].machine_index == NONE);
    network_test_suite_end();
    for (int count = 2; count <= 32; count += count == 2 ? 14 : 16) {
        reset_game();
        CHECK(network_test_suite_begin("network_players", "bloodgulch", "slayer", count, 1, 37, 7));
        network_test.setup_seconds = 1.0f; actual_lobby_map_set();
        network_test.setup_seconds = 2.0f; actual_lobby_after_map();
        CHECK(network_test.mode == _network_test_host && network_test.players_wanted == 1);
        CHECK(server.game.machine_count == count && server.game.player_count == count);
        CHECK(server.joined_transport_machines == 1 && local_requests == 1);
        for (int i = 0; i < count; i++) {
            CHECK(server.game.machines[i].machine_index == i);
            CHECK(server.game.players[i].machine_index == i && server.game.players[i].controller_index == 0);
            CHECK(i == 0 || server.client_machines[i].flags == 0);
        }
        actual_lobby_after_map();
        CHECK(server.game.machine_count == count && server.game.player_count == count);
        now_ms = 250;
        CHECK(network_game_server_flush_game_data_pregame(&server));
        CHECK(sent_roster_count == count);
        network_test_suite_end();
    }
    for (int failure = 0; failure < 4; failure++) {
        reset_game();
        CHECK(network_test_suite_begin("network_players", "sidewinder", "slayer", 32, 1, 37, 7));
        if (failure == 0) server.game.maximum_players = 31;
        if (failure == 1) server.game.machines[15].machine_index = 15;
        if (failure == 2) force_slots_held = -5; /* fail after four successful insertions */
        if (failure == 3) { force_settings_failure = TRUE; now_ms = 250; }
        CHECK(!network_test_suite_add_synthetic_players(&server));
        CHECK(server.game.player_count == 1 && server.game.machine_count == 1);
        CHECK(server.game.players[0].machine_index == 0);
        for (int i = 1; i < 32; i++) {
            CHECK(server.game.players[i].machine_index == NONE);
            CHECK(server.game.machines[i].machine_index == (failure == 1 && i == 15 ? 15 : NONE));
        }
        network_test_suite_end();
    }
    return failures != 0;
}
'''


def test_synthetic_player_reaches_local_client_settings() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise SkipTest("A C compiler is required for the synthetic roster probe")
    state_start = NETWORK_TEST.index("static struct\n{")
    state_end = NETWORK_TEST.index("} network_test;", state_start) + len("} network_test;")
    source = HARNESS
    replacements = {
        "__PLAYER_VALID__": function(GAME_MANAGER, "boolean network_player_is_valid("),
        "__ADD_MACHINE__": function(GAME_MANAGER, "boolean network_game_add_machine("),
        "__ADD_PLAYER__": function(GAME_MANAGER, "boolean network_game_add_player("),
        "__INVALIDATE_PLAYER__": function(GAME_MANAGER, "void network_game_invalidate_player("),
        "__INVALIDATE_MACHINE__": function(GAME_MANAGER, "void network_game_invalidate_machine("),
        "__PLAYER_IS_VALID__": function(GAME_MANAGER, "boolean network_game_player_is_valid("),
        "__REMOVE_PLAYER__": function(GAME_MANAGER, "boolean network_game_remove_player("),
        "__REMOVE_MACHINE__": function(GAME_MANAGER, "boolean network_game_remove_machine("),
        "__CLIENT_JOINED__": function(GAME_SERVER, "boolean network_game_server_client_machine_is_joined_to_game("),
        "__ENOUGH_MACHINES__": function(GAME_SERVER, "boolean server_has_enough_machines("),
        "__NETWORK_TEST_STATE__": NETWORK_TEST[state_start:state_end],
        "__SUITE_FUNCTIONS__": suite_functions(),
        "__VARIANT_FUNCTION__": function(NETWORK_TEST, "static boolean network_test_variant("),
        "__MAP_SET_BRANCH__": lobby_branch(),
        "__AFTER_MAP_BRANCH__": after_map_branch(),
        "__FLUSH_SETTINGS__": function(GAME_MESSAGES, "boolean network_game_server_flush_game_data_pregame("),
        "__SEND_SETTINGS__": function(GAME_MESSAGES, "boolean network_game_server_send_game_data_pregame("),
    }
    for key, value in replacements.items():
        source = source.replace(key, value)
    with tempfile.TemporaryDirectory(prefix="mp-synthetic-roster-") as temporary:
        path = Path(temporary) / "roster.c"
        binary = Path(temporary) / "roster"
        path.write_text(source)
        subprocess.run([compiler, "-O2", "-std=gnu89", "-fms-extensions", "-Wno-c99-extensions", "-Wall", str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    test_synthetic_player_reaches_local_client_settings()
