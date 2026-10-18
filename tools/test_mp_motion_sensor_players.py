#!/usr/bin/env python3
"""Run the production motion sensor scan with three and four local players."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "source/interface/motion_sensor.c"


def function_source(source: str) -> str:
    start = source.index("static void motion_sensor_update(\n\tvoid)\n{")
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise ValueError("Unclosed motion_sensor_update")


HARNESS = r'''
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define long int /* Match the guest's 32-bit long on this native test host. */
#define NONE -1
#define TRUE 1
#define FALSE 0
#define MOTION_SENSOR_HISTORY_COUNT 2
#define MOTION_SENSOR_UPDATE_PERIOD 5
#define MAXIMUM_LOCAL_PLAYERS 4
#define MAXIMUM_MOTION_SENSOR_BLIPS 32
#define _object_mask_unit 1
#define _blip_type_none 0
typedef int boolean;
typedef float real;
typedef struct { real x, y, z; } real_point3d;
typedef struct { real i, j, k; } real_vector3d;
struct motion_sensor_blip { char type; };
struct motion_sensor_datum { struct motion_sensor_blip blips[32]; int blip_count; };
struct motion_sensor_player { struct motion_sensor_datum sensor_data[2]; int unit_indices[32]; };
struct motion_sensor_globals_definition {
    struct motion_sensor_player players[4];
    int last_update_time;
    short active_sensor_index;
    boolean update;
};
struct hud_globals_definition { struct { real motion_sensor_range; } defaults; };
struct player_datum { int unit_index; };
struct object_iterator { int index, seen; };
static struct motion_sensor_globals_definition state;
static struct motion_sensor_globals_definition *motion_sensor_globals = &state;
static struct hud_globals_definition hud_state = { .defaults = { 100.0f } };
static struct hud_globals_definition *hud_globals = &hud_state;
static struct player_datum players[4];
static int active_players;
static int get_return_eip(void) { return 0; }
static void *csmemset(void *dest, int value, size_t size) { return memset(dest, value, size); }
static void *csmemcpy(void *dest, const void *source, size_t size) { return memcpy(dest, source, size); }
static int game_engine_running(void) { return 1; }
static int game_time_get(void) { return 0; }
static short local_player_count(void) { return (short)active_players; }
static short local_player_get_next(short current) { return current + 1 < active_players ? current + 1 : NONE; }
static int local_player_get_player_index(short local) {
    if (local < 0 || local >= active_players) abort();
    return local;
}
static struct player_datum *player_get(int index) { return &players[index]; }
static void unit_get_camera_position(int unit, real_point3d *out) {
    *out = (real_point3d){ (real)unit, 0, 0 };
}
static void game_engine_motion_sensor_viewer(short local) { (void)local; }
static void object_iterator_new(struct object_iterator *iter, int mask, boolean active) {
    (void)mask; (void)active; iter->index = 1; iter->seen = 0;
}
static int object_iterator_next(struct object_iterator *iter) { return iter->seen++ == 0; }
static int should_track_object(int index) { (void)index; return 1; }
static int should_draw_object(int index) { (void)index; return 1; }
static void object_get_bounding_sphere(int index, real_point3d *center, real *radius) {
    (void)index; *center = (real_point3d){ 0, 0, 0 }; *radius = 1;
}
static real magnitude_squared3d(const real_vector3d *v) {
    return v->i * v->i + v->j * v->j + v->k * v->k;
}
static void motion_sensor_blip_set_type_and_size(struct motion_sensor_blip *blip, int object, short local) {
    (void)object; (void)local; blip->type = 1;
}
#define match_vassert(file, line, condition, detail) do { \
    (void)(file); (void)(line); if (!(condition)) abort(); \
} while (0)
__PRODUCTION_FUNCTION__
int main(void) {
    for (active_players = 3; active_players <= 4; active_players++) {
        memset(&state, 0, sizeof(state));
        for (int index = 0; index < active_players; index++) players[index].unit_index = 1;
        motion_sensor_update();
        for (int index = 0; index < active_players; index++) {
            if (state.players[index].sensor_data[1].blip_count != 1) {
                fprintf(stderr, "player %d of %d has %d blips\n", index, active_players,
                    state.players[index].sensor_data[1].blip_count);
                return 1;
            }
        }
    }
    return 0;
}
'''


def run_probe(path: Path) -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise SkipTest("A C compiler is required for the motion sensor probe")
    production = function_source(path.read_text())
    with tempfile.TemporaryDirectory(prefix="mp-motion-sensor-") as temporary:
        source = Path(temporary) / "motion_sensor.c"
        binary = Path(temporary) / "motion_sensor"
        source.write_text(HARNESS.replace("__PRODUCTION_FUNCTION__", production))
        subprocess.run([
            compiler, "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_three_and_four_local_players() -> None:
    run_probe(SOURCE)


if __name__ == "__main__":
    run_probe(Path(sys.argv[1]) if len(sys.argv) > 1 else SOURCE)
