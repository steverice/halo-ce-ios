#!/usr/bin/env python3
"""Exercise the production private match start and reset without game data."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "port/linux/game/network_test.c").read_text()


def function_source(signature: str) -> str:
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
    raise ValueError(f"Unclosed function: {signature}")


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int boolean;
typedef float real;
#define TRUE 1
enum { _network_test_off, _network_test_host };
enum { _game_connection_local, _game_connection_network_server = 2 };
static const char *configured = "";
static int connection, aborts;
static const char *__attribute__((unused)) config_string(const char *name) {
    return !strcmp(name, "debug.network_test") ? configured : "";
}
static int __attribute__((unused)) game_connection(void) { return connection; }
static void __attribute__((unused)) network_game_abort(void) { aborts++; }
__NETWORK_STATE__
__BEGIN_FUNCTION__
__END_FUNCTION__
static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "network suite line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
int main(void) {
    const char *bad_simulations[] = { NULL, "", "invalid", "other" };
    for (int i = 0; i < 4; i++) {
        CHECK(!network_test_suite_begin(bad_simulations[i], "chillout", "slayer", 4, 1, 1000, 0));
        CHECK(network_test.mode == _network_test_off && !network_test.suite_owned);
    }
    CHECK(!network_test_suite_begin("local_controllers", "../bloodgulch", "slayer", 4, 1, 1000, 0));
    CHECK(!network_test_suite_begin("local_controllers", "chillout", "ctf", 4, 1, 1000, 0));
    CHECK(!network_test_suite_begin("local_controllers", "chillout", "slayer", 4, 2, 1000, 0));
    CHECK(!network_test_suite_begin("network_players", "chillout", "slayer", 33, 1, 1000, 0));
    CHECK(!network_test_suite_begin("local_controllers", "chillout", "slayer", 4, 1, 0, 0));
    CHECK(!network_test_suite_begin("local_controllers", "chillout", "slayer", 4, 1, 1000, 121));
    CHECK(network_test.mode == _network_test_off && !network_test.suite_owned);
    configured = "join";
    CHECK(!network_test_suite_begin("local_controllers", "chillout", "slayer", 4, 1, 1000, 0));
    CHECK(network_test.mode == _network_test_off && !network_test.suite_owned);
    configured = "";
    CHECK(network_test_suite_begin("local_controllers", "chillout", "slayer", 4, 1, 1000, 0));
    CHECK(network_test_suite_begin("local_controllers", "chillout", "slayer", 4, 1, 1000, 0));
    CHECK(network_test.checked && network_test.suite_owned && network_test.mode == _network_test_host);
    CHECK(!strcmp(network_test.map_name, "chillout") && !strcmp(network_test.variant_name, "slayer"));
    CHECK(network_test.players_wanted == 4 && network_test.start_delay == 15.0f);
    CHECK(network_test.score_to_win == 1000 && network_test.shoot_interval == 5.0f &&
        network_test.kill_interval == 25.0f && !network_test.public_game);
    unsigned char snapshot[sizeof(network_test)];
    memcpy(snapshot, &network_test, sizeof(network_test));
    CHECK(!network_test_suite_begin("network_players", "chillout", "slayer", 2, 1, 1000, 0));
    CHECK(!network_test_suite_begin("local_controllers", "chillout", "slayer", 4, 1, 25, 7));
    CHECK(!memcmp(snapshot, &network_test, sizeof(network_test)));
    connection = _game_connection_network_server;
    network_test_suite_end();
    CHECK(network_test.checked && network_test.mode == _network_test_off && !network_test.suite_owned);
    CHECK(aborts == 1);
    network_test_suite_end();
    CHECK(aborts == 1);
    connection = _game_connection_local;
    CHECK(network_test_suite_begin("local_controllers", "chillout", "slayer", 4, 1, 1000, 0));
    network_test_suite_end();
    CHECK(aborts == 1);
    for (int players = 2; players <= 32; players++) {
        CHECK(network_test_suite_begin("network_players", "bloodgulch", "slayer", players, 1, 37, 7));
        CHECK(network_test.players_wanted == 1);
        CHECK(network_test_suite_begin("network_players", "bloodgulch", "slayer", players, 1, 37, 7));
        memcpy(snapshot, &network_test, sizeof(network_test));
        CHECK(!network_test_suite_begin("network_players", "sidewinder", "slayer", players, 1, 37, 7));
        CHECK(!network_test_suite_begin("network_players", "bloodgulch", "slayer", players == 32 ? 2 : players + 1, 1, 37, 7));
        CHECK(!memcmp(snapshot, &network_test, sizeof(network_test)));
        network_test_suite_end();
    }
    return failures ? 1 : 0;
}
'''


def test_private_match_lifecycle() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise SkipTest("A C compiler is required for the network suite probe")
    state_start = SOURCE.index("static struct\n{")
    state_end = SOURCE.index("} network_test;", state_start) + len("} network_test;")
    source = HARNESS.replace("__NETWORK_STATE__", SOURCE[state_start:state_end])
    begin = function_source("int network_test_suite_begin(")
    source = source.replace("__BEGIN_FUNCTION__", begin)
    source = source.replace("__END_FUNCTION__", function_source("void network_test_suite_end(void)"))
    with tempfile.TemporaryDirectory(prefix="mp-network-suite-") as temporary:
        source_path = Path(temporary) / "network_suite.c"
        binary = Path(temporary) / "network_suite"
        source_path.write_text(source)
        subprocess.run([compiler, "-O2", "-Wall", "-Wextra", "-Werror", str(source_path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    test_private_match_lifecycle()
