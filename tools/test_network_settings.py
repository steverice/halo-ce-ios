"""Exercise the production options reader and fragment receiver with synthetic records."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def definition(path, marker):
    source = (ROOT / path).read_text()
    start = source.index(marker)
    opening = source.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if marker.startswith("struct "):
        end = source.index(";", end) + 1
    return source[start:end]


def compile_run(tmp_path, program):
    source = tmp_path / "probe.c"
    source.write_text(program)
    executable = tmp_path / "probe"
    subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function",
                    str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


def test_saved_options_preserve_legacy_and_expanded_layouts(tmp_path):
    structure = definition("source/game/game_engine.h", "struct game_variant_options\n{")
    # The first occurrence is the forward declaration; select the definition.
    text = (ROOT / "source/saved games/playlist_profile.c").read_text()
    start = text.rindex("static boolean playlist_profile_options_from_block(")
    reader = definition_text(text[start:])
    program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "CONTENT_HEADER"
typedef unsigned char byte, boolean;
typedef unsigned short word;
#define FALSE 0
#define TRUE 1
#define NUMBER_OF_VARIANT_VEHICLES 6
#define NUMBER_OF_FRIENDLY_FIRE_MODES 4
#define NUMBER_OF_RADAR_PLAYERS 3
#define NUMBER_OF_LOADOUTS 2
#define NUMBER_OF_LOADOUT_WEAPONS 11
#define _loadout_weapon_rocket_launcher 6
#define PLAYLIST_PROFILE_OPTIONS_OFFSET 0x100
#define PLAYLIST_PROFILE_OPTIONS_MAGIC 0x4F565047
#define PLAYLIST_PROFILE_OPTIONS_VERSION 2
#define csmemcpy memcpy
#define csmemcmp memcmp
#define PIN(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
typedef unsigned char XCALCSIG_SIGNATURE[16];
struct game_variant { int unused; };
''' + structure + r'''
struct playlist_profile_options_header { uint32_t magic; word version, size; };
static void game_variant_options_default(struct game_variant const *v, struct game_variant_options *o) {
    (void)v; memset(o, 0, sizeof(*o)); halo_network_options_defaults(o->mode_options);
}
/* A deterministic checksum boundary tests the reader's length and placement.
   The platform signature algorithm itself is outside the options codec. */
static void saved_game_file_generate_checksum(byte *data, unsigned long size, XCALCSIG_SIGNATURE *sum) {
    memset(sum, 0, 16); for (unsigned i = 0; i < size; i++) (*sum)[i % 16] ^= data[i];
}
''' + reader + r'''
int main(void) {
    byte block[512] = {0};
    struct game_variant_options expected, decoded;
    struct playlist_profile_options_header header = {PLAYLIST_PROFILE_OPTIONS_MAGIC, 1, 28};
    assert(sizeof(expected) == 44 && offsetof(struct game_variant_options, mode_options) == 28);
    game_variant_options_default(0, &expected);
    expected.time_limit = 17; expected.friendly_fire = 2;
    expected.vehicle_counts[1][5] = 4; expected.primary_weapon = 3;
    for (int version = 1; version <= 2; version++) {
        header.version = version; header.size = version == 1 ? 28 : 44;
        expected.mode_options[3] = version == 1 ? 150 : 175;
        memcpy(block + 256, &header, sizeof(header));
        memcpy(block + 264, &expected, header.size);
        saved_game_file_generate_checksum(block + 256, 8 + header.size,
            (XCALCSIG_SIGNATURE *)(block + 264 + header.size));
        assert(playlist_profile_options_from_block(block, &decoded));
        assert(!memcmp(&expected, &decoded, sizeof(expected)));
        block[264 + header.size] ^= 1;
        assert(!playlist_profile_options_from_block(block, &decoded));
    }
    header.version = 2; header.size = 28;
    memcpy(block + 256, &header, sizeof(header));
    assert(!playlist_profile_options_from_block(block, &decoded));
    header.version = 1; header.size = 44;
    memcpy(block + 256, &header, sizeof(header));
    assert(!playlist_profile_options_from_block(block, &decoded));
    return 0;
}
'''
    compile_run(tmp_path, program.replace("CONTENT_HEADER", str(ROOT / "port/linux/include/halo_network_content.h")))


def definition_text(source):
    opening = source.index("{")
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[:end]


def test_settings_fragments_convert_only_legacy_hosts(tmp_path):
    text = (ROOT / "source/networking/network_client_message_handler.c").read_text()
    receiver = definition_text(text[text.index("static boolean network_game_client_receive_game_settings_piece("):])
    program = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "CONTENT_HEADER"
#include "VERSION_HEADER"
typedef unsigned char byte;
typedef unsigned short word;
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define csmemcpy memcpy
#define network_event(...) ((void)0)
struct network_game { byte raw[HALO_PORT_NETWORK_GAME_SIZE]; };
struct network_game_client { int unused; };
struct message_server_game_settings_update {
    word total_size, offset, length; byte data[HALO_PORT_NETWORK_GAME_SETTINGS_FRAGMENT_SIZE];
};
static struct network_game network_game_client_settings_staging;
static long network_game_client_settings_staging_size;
static unsigned host_version = 38;
static int applied;
static struct network_game result;
static unsigned network_game_host_version(void) { return host_version; }
static boolean network_game_client_game_settings_updated(struct network_game_client *c, struct network_game *g) {
    (void)c; result = *g; applied++; return TRUE;
}
''' + receiver + r'''
int main(void) {
    struct message_server_game_settings_update piece = {0};
    /* 29 to 34 send the 28-byte options block; 35 to 38 the 44-byte one */
    for (unsigned v = 29; v <= 38; v++) {
        host_version = v; applied = 0;
        unsigned total = v >= 35 ? 13136 : 13120;
        for (unsigned offset = 0; offset < total; offset += 3584) {
            piece.total_size = total; piece.offset = offset;
            piece.length = total - offset < 3584 ? total - offset : 3584;
            memset(piece.data, 0xA5, sizeof(piece.data));
            assert(network_game_client_receive_game_settings_piece(0, &piece));
        }
        assert(applied == 1 && result.raw[13088] == 0xA5);
        assert(result.raw[13132] == 0xA5);
        assert(result.raw[13119] == (v >= 35 ? 0xA5 : 150));
        piece.offset = 0; piece.length = 1; piece.total_size = total == 13136 ? 13120 : 13136;
        assert(!network_game_client_receive_game_settings_piece(0, &piece));
        piece.total_size = total; piece.offset = total - 1; piece.length = 2;
        assert(!network_game_client_receive_game_settings_piece(0, &piece));
        piece.offset = 0; piece.length = 3585;
        assert(!network_game_client_receive_game_settings_piece(0, &piece));
    }
    return 0;
}
'''
    compile_run(tmp_path, program.replace("CONTENT_HEADER", str(ROOT / "port/linux/include/halo_network_content.h")).replace(
        "VERSION_HEADER", str(ROOT / "port/linux/include/halo_network_version.h")))


def test_joined_settings_refuse_unsupported_rules_before_precaching(tmp_path):
    handler = definition("source/networking/network_client_manager.c", "boolean network_game_client_game_settings_updated(")
    program = r'''
#include <assert.h>
#include <string.h>
#include "CONTENT_HEADER"
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define MAXIMUM_NETWORK_MACHINE_COUNT 128
#define MAXIMUM_NUMBER_OF_PLAYERS 128
#define NUMBER_OF_GAME_DIFFICULTY_LEVELS 4
#define VALID_INDEX(v, n) ((unsigned)(v) < (n))
#define game_engine_none 0
#define last_usable_game_engine_index 5
#define _error_network_failed_to_join_game 7
#define match_assert(...) ((void)0)
#define network_event(...) ((void)0)
#define csmemcpy memcpy
#define csstrcmp strcmp
struct network_game {
    int machine_count, player_count, maximum_players, difficulty;
    struct { char name[128]; long version; } map;
    struct { int game_engine_index; } variant;
    struct { unsigned char mode_options[16]; } variant_options;
    struct { int game_objects_loaded; } local_data;
};
struct network_game_client { struct network_game game; };
static int precached, told, generic;
static const char *reason;
static int network_game_client_map_name_is_valid(const char *s, unsigned n) { return memchr(s, 0, n) != 0; }
static int network_game_client_game_record_is_valid(struct network_game *g) { (void)g; return 1; }
static int network_game_is_splitscreen_local(void) { return 0; }
static int cache_files_map_present(const char *s, unsigned long v) { (void)s; (void)v; precached++; return 1; }
static int cache_files_map_plays_multiplayer(const char *s, char *b) { (void)s; (void)b; return 1; }
static void cache_files_show_multiplayer_unavailable(const char *s, const char *b) { (void)s; (void)b; }
static void main_set_multiplayer_map_name(const char *s) { (void)s; }
static void platform_show_message(const char *title, const char *message) { (void)title; told++; reason = message; }
static void display_error_when_main_menu_loaded(int e) { generic = e; }
''' + handler + r'''
int main(void) {
    struct network_game_client client = {0};
    struct network_game settings = {0};
    settings.maximum_players = 128; settings.variant.game_engine_index = 2;
    strcpy(settings.map.name, "bloodgulch");
    halo_network_options_defaults(settings.variant_options.mode_options);
    for (int mode = 1; mode <= 3; mode++) {
        settings.variant_options.mode_options[0] = mode;
        assert(!network_game_client_game_settings_updated(&client, &settings));
        assert(precached == 0 && told == mode && reason && generic == 7);
        assert(client.game.maximum_players == 0);
    }
    settings.variant_options.mode_options[0] = 0;
    settings.variant_options.mode_options[1] = 1;
    assert(!network_game_client_game_settings_updated(&client, &settings));
    assert(strstr(reason, "Hardcore") && precached == 0);
    settings.variant_options.mode_options[1] = 0;
    settings.variant.game_engine_index = 6;
    assert(!network_game_client_game_settings_updated(&client, &settings));
    assert(precached == 0);
    settings.variant.game_engine_index = 2;
    settings.variant_options.mode_options[3] = 175;
    assert(network_game_client_game_settings_updated(&client, &settings));
    assert(precached == 1 && client.game.maximum_players == 128);
    return 0;
}
'''
    compile_run(tmp_path, program.replace("CONTENT_HEADER", str(ROOT / "port/linux/include/halo_network_content.h")))


def test_original_menu_preserves_options_and_refuses_imported_modes(tmp_path):
    text = (ROOT / "source/interface/ui_widget_event_handler_functions.c").read_text()
    marker = "static boolean multiplayer_profile_set_for_game("
    start = text.rindex(marker)
    handler = definition_text(text[start:])
    program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "CONTENT_HEADER"
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define match_vassert(file, line, value, message) assert(value)
#define fopen(...) fixture_automation_file()
struct game_variant { int value; };
struct game_variant_options { unsigned char mode_options[16]; };
struct event_record { int unused; };
struct widget_instance {
    int definition_tag_index;
    struct widget_instance *child;
    struct { int selected_index; } data3C;
    int generated_count;
    void *generated_list;
};
struct ui_widget_definition { int child_count, type; };
static struct ui_widget_definition definitions[] = {{1, 0}, {3, 0}, {3, 2}};
static struct game_variant_options selected, applied;
static int told, hosting, automate;
static FILE *fixture_automation_file(void) {
    if (!automate) return 0;
    FILE *file = tmpfile(); assert(file); fputs("Slayer\n", file); rewind(file); return file;
}
static struct ui_widget_definition *ui_widget_definition_get(int i) { return &definitions[i]; }
static void ui_play_audio_feedback_sound(int i) { (void)i; }
static void display_error_deferred(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; }
static int playlist_profile_get(long i, struct game_variant *v) { (void)i; v->value = 1; return 1; }
static int playlist_profile_get_options(long i, struct game_variant_options *v) { (void)i; *v = selected; return 1; }
static void *global_network_game_server_get(void) { return &hosting; }
static int saved_game_file_get_path_to_enclosing_directory(long i, char *p) { (void)i; (void)p; return 0; }
static void saved_game_file_remember_last_used_multiplayer_variant_directory(char *p) { (void)p; }
static struct game_variant *game_engine_get_variant_by_name(struct game_variant *v, char *s) { (void)s; v->value = 42; return v; }
static void game_variant_options_default(struct game_variant const *v, struct game_variant_options *o) { (void)v; halo_network_options_defaults(o->mode_options); }
static void player_ui_set_game_variant(struct game_variant *v) { (void)v; memset(&applied, 0, sizeof(applied)); }
static void player_ui_set_game_variant_options(struct game_variant_options const *v) { applied = *v; }
static void network_game_server_change_game_variant(void *s, struct game_variant *v) { (void)s; (void)v; hosting++; }
static void platform_show_message(char const *t, char const *m) { (void)t; assert(m); told++; }
static void error(int i, char const *s) { (void)i; (void)s; }
''' + handler + r'''
int main(void) {
    long index = 0x80000001;
    struct widget_instance list = {.definition_tag_index=2, .generated_count=1, .generated_list=&index};
    struct widget_instance screen = {.definition_tag_index=1, .child=&list};
    struct widget_instance root = {.definition_tag_index=0, .child=&screen};
    halo_network_options_defaults(selected.mode_options);
    selected.mode_options[0] = 2;
    assert(!multiplayer_profile_set_for_game(&root, 0, 0));
    assert(hosting == 0 && told == 1);
    selected.mode_options[0] = 0; selected.mode_options[1] = 1;
    assert(!multiplayer_profile_set_for_game(&root, 0, 0));
    assert(hosting == 0 && told == 2);
    selected.mode_options[1] = 0; selected.mode_options[3] = 175;
    assert(multiplayer_profile_set_for_game(&root, 0, 0));
    assert(hosting == 1 && !memcmp(&selected, &applied, sizeof(selected)));
    automate = 1;
    assert(multiplayer_profile_set_for_game(&root, 0, 0));
    assert(hosting == 2 && applied.mode_options[3] == 150);
    return 0;
}
'''
    compile_run(tmp_path, program.replace("CONTENT_HEADER", str(ROOT / "port/linux/include/halo_network_content.h")))
