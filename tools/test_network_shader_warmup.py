"""Run the network map loader with the renderer's real map-attribution code.

Removing the network warmup or using a stale solo map must leave this probe red.
The game engine and GPU are replaced at their boundaries; no game data is needed.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_network_maps_select_shader_capture_before_the_first_frame(tmp_path):
    loader = function((ROOT / "source/networking/network_game_manager.c").read_text(),
                      "boolean network_game_create_game_objects(")
    warm = function((ROOT / "port/linux/src/d3d8_device.c").read_text(),
                    "void halo_shader_list_warm(")
    # Keep the real basename selection and disabled-GPU return. The rest compiles
    # GPU programs, which this engine-boundary probe does not have a device for.
    warm = warm[:warm.index("\t/* a run that dumps")] + "}\n"
    program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define NETWORK_GAME_PLAYER_SLOTS 2
#define match_assert(file, line, value) assert(value)
#define csstrncpy strncpy
enum { _game_connection_network_client, _game_connection_network_server, _game_connection_film_playback };
struct game_options { char map_name[256]; int difficulty, random_seed; };
struct network_player { int player_list_index, valid; };
struct network_game {
    struct { char name[256]; } map;
    int difficulty, random_seed;
    struct { int game_engine_index; } variant;
    int variant_options;
    struct { boolean game_objects_loaded; } local_data;
    struct network_player players[NETWORK_GAME_PLAYER_SLOTS];
};
static struct { char map[256]; } shader_list;
static struct { int gl_ready; } device;
static int connection, load_ok, spawn_ok, initialized, spawned;
static int config_boolean(const char *key) { (void)key; return 0; }
static void game_options_new(struct game_options *o) { memset(o, 0, sizeof(*o)); }
static int game_connection(void) { return connection; }
static int network_game_get_random_seed(void) { return 7; }
static void game_precache_new_map(const char *name, int force) { (void)name; (void)force; }
static void main_menu_unload(void) {}
static int game_in_progress(void) { return 0; }
static int game_map_loaded(void) { return 1; }
static void game_dispose_from_old_map(void) {}
static void game_unload(void) {}
static void game_set_game_variant(void *v) { (void)v; }
static void game_set_game_variant_options(void *v) { (void)v; }
static int game_load(struct game_options *o) { (void)o; return load_ok; }
static void game_initialize_for_new_map(void) { initialized++; }
static int network_player_is_valid(struct network_player *p) { return p->valid; }
static int network_game_spawn_player(struct network_player *p) { (void)p; spawned++; return spawn_ok; }
static void error(int level, const char *message) { (void)level; (void)message; }
''' + warm + loader + r'''
int main(void) {
    struct network_game game = {0};
    game.players[0].valid = game.players[1].valid = 1;
    game.variant.game_engine_index = 1;
    for (connection = 0; connection < 3; connection++) {
        const char *maps[] = { "levels\\test\\chillout\\chillout", "levels/test/bloodgulch/bloodgulch", "chillout" };
        const char *names[] = { "chillout", "bloodgulch", "chillout" };
        for (int i = 0; i < 3; i++) {
            strcpy(shader_list.map, "ui");
            strcpy(game.map.name, maps[i]);
            game.local_data.game_objects_loaded = 0;
            load_ok = spawn_ok = 1;
            initialized = spawned = 0;
            assert(network_game_create_game_objects(&game));
            assert(initialized == 1 && spawned == 2);
            /* The next draw records under the actual network map, even when
               warming is disabled or the map has no bundled list yet. */
            assert(!strcmp(shader_list.map, names[i]));
        }
    }
    connection = _game_connection_network_client;
    strcpy(shader_list.map, "ui");
    game.local_data.game_objects_loaded = 0;
    load_ok = 0;
    assert(!network_game_create_game_objects(&game));
    assert(!strcmp(shader_list.map, "ui"));
    load_ok = 1;
    spawn_ok = 0;
    assert(!network_game_create_game_objects(&game));
    assert(!strcmp(shader_list.map, "ui"));
    puts("PASS: network map attribution, repeat loads and failed loads");
}
'''
    source, binary = tmp_path / "probe.c", tmp_path / "probe"
    source.write_text(program)
    subprocess.run(["xcrun", "clang", "-Wall", "-Werror", "-Wno-unused-variable", "-fsanitize=address,undefined",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        test_network_maps_select_shader_capture_before_the_first_frame(Path(directory))
