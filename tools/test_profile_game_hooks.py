"""Exercise production game eligibility and LOD consumers without game assets."""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def compile_probe(tmp_path, text):
    source = tmp_path / "probe.c"
    source.write_text(text)
    binary = tmp_path / "probe"
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary), "-lm"], check=True)
    return binary


@pytest.fixture
def eligibility_probe(tmp_path):
    actual = function((ROOT / "source/game/game.c").read_text(), "void config_profile_game_safe_point(")
    return compile_probe(tmp_path, r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "''' + str(ROOT / "port/linux/src/port_config.h") + r'''"
enum { _game_connection_local, _game_connection_network_client,
       _game_connection_network_server, _game_connection_film_playback };
static const char *flag = "";
static int split, connection, playing, isolated, benchmark_isolated, seen_point, seen_context;
int config_boolean(const char *name) { return !strcmp(name, flag); }
long config_integer(const char *name) { return !strcmp(name, flag); }
double config_real(const char *name) { return !strcmp(name, flag); }
const char *config_string(const char *name) { return !strcmp(name, flag) ? "active" : ""; }
int config_benchmark_isolated(void) { return benchmark_isolated; }
unsigned char input_replay_playing(void) { return playing; }
unsigned char input_replay_sim_isolated(void) { return isolated; }
unsigned char pc_menu_split_players(void) { return split; }
static int game_connection(void) { return connection; }
void config_profile_safe_point(int point, int context) { seen_point=point; seen_context=context; }
''' + actual + r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    for (int point=1; point<=5; point++) {
        config_profile_game_safe_point(point);
        assert(seen_point == point && seen_context == 0);
    }
    split=1;
    config_profile_game_safe_point(1);
    assert(seen_context == CONFIG_PROFILE_FROZEN);
    split=0; connection=_game_connection_network_client;
    config_profile_game_safe_point(2);
    assert(seen_context == CONFIG_PROFILE_NETWORKED);
    connection=_game_connection_local;
    if (!strcmp(argv[1], "runner")) assert(setenv("HALO_RUNNER","1",1)==0);
    else if (!strcmp(argv[1], "playing")) playing=1;
    else if (!strcmp(argv[1], "isolated")) isolated=1;
    else if (!strcmp(argv[1], "benchmark_isolated")) benchmark_isolated=1;
    else if (!strcmp(argv[1], "film")) connection=_game_connection_film_playback;
    else flag=argv[1];
    config_profile_game_safe_point(3);
    assert(seen_context & CONFIG_PROFILE_FROZEN);
    flag=""; playing=isolated=benchmark_isolated=0; connection=_game_connection_local;
    assert(unsetenv("HALO_RUNNER")==0);
    config_profile_game_safe_point(1);
    assert(seen_context == CONFIG_PROFILE_FROZEN);
}
''')


@pytest.mark.parametrize("flag", [
    "debug.benchmark", "debug.benchmark_suite", "debug.input_record", "debug.input_replay",
    "debug.controller_record", "debug.controller_replay", "debug.test_input", "debug.test_controller",
    "debug.network_test", "debug.render_capture", "debug.gpu_shader_replay", "debug.test_text_answer",
    "debug.fixed_timestep", "debug.multiplayer_benchmark_probe", "debug.test_extra_scene",
    "debug.test_reclaim_sockets", "debug.test_theater_reopen", "debug.exit_after",
    "debug.test_win_level", "debug.screenshot_every", "playing", "isolated", "benchmark_isolated", "film", "runner",
])
def test_reproducible_run_stays_frozen(eligibility_probe, flag):
    subprocess.run([str(eligibility_probe), flag], check=True)


def test_model_lod_refresh_preserves_explicit_benchmark_override(tmp_path):
    text = (ROOT / "source/models/models.c").read_text()
    start = text.index("static int model_lod_setting")
    end = text.index("\n/* port: debug.gpu_stats", start)
    binary = compile_probe(tmp_path, r'''
#include <assert.h>
#include <string.h>
typedef int boolean;
static const char *setting = "max";
static unsigned long revision;
static int reads;
static const char *config_string(const char *name) { (void)name; reads++; return setting; }
static unsigned long config_changes(void) { return revision; }
''' + text[start:end] + r'''
int main(void) {
    assert(halo_model_lod_override_state() == -1);
    assert(halo_model_lod_effective() == 1 && reads == 1);
    setting="auto"; revision++;
    assert(halo_model_lod_effective() == 0 && reads == 2);
    assert(halo_model_lod_effective() == 0 && reads == 2);
    halo_model_lod_set(1);
    assert(halo_model_lod_override_state() == 1);
    revision++;
    assert(halo_model_lod_effective() == 1 && reads == 2);
    halo_model_lod_set(-1);
    assert(halo_model_lod_override_state() == -1);
    assert(halo_model_lod_effective() == 0 && reads == 3);
}
''')
    subprocess.run([str(binary)], check=True)


def test_level_target_and_sampler_defaults_refresh(tmp_path):
    text = (ROOT / "port/linux/src/d3d8_device.c").read_text()
    actual = function(text, "static void offscreen_target_scale(") + "\n" + function(text, "static long anisotropy_in_force(")
    binary = compile_probe(tmp_path, r'''
#include <assert.h>
#include <string.h>
typedef unsigned long DWORD;
enum { D3DFMT_R5G6B5=1, D3DFMT_LIN_R5G6B5 };
static unsigned long revision;
static long shadow=256, anisotropy=2, benchmark_shadow_size, knob_shadow_size;
static long benchmark_anisotropy, knob_anisotropy;
static int effect=1, knob_effect_level=-1;
static unsigned long config_changes(void) { return revision; }
static long config_integer(const char *name) { return !strcmp(name,"display.shadow_map_size") ? shadow : anisotropy; }
enum { CONFIG_SCENE_GAMEPLAY };
/* (the workload context's value, when a map is variable: [variable.display]) */
static long config_scene_integer(const char *name, int scene) { (void)scene; return config_integer(name); }
static unsigned long knob_release_frame;
static struct { unsigned long frame; } device = { 1000 };
static const char *config_string(const char *name) { (void)name; return effect==1 ? "half" : "full"; }
static int effect_resolution_level(const char *value) { return !strcmp(value,"half") ? 1 : 2; }
static void screen_scale_dense(float scale[2]) { scale[0]=scale[1]=2; }
''' + actual + r'''
int main(void) {
    float scale[2]={1,1};
    offscreen_target_scale(128,128,D3DFMT_R5G6B5,scale);
    assert(scale[0]==2 && anisotropy_in_force()==2);
    assert(!knob_release_frame);
    shadow=512; anisotropy=8; effect=2; revision++;
    offscreen_target_scale(128,128,D3DFMT_R5G6B5,scale);
    assert(scale[0]==4 && anisotropy_in_force()==8);
    /* (the old size's targets are freed two seconds on) */
    assert(knob_release_frame==1120);
    offscreen_target_scale(320,240,0,scale);
    assert(scale[0]==4);
    benchmark_shadow_size=128; benchmark_anisotropy=1; revision++;
    offscreen_target_scale(128,128,D3DFMT_R5G6B5,scale);
    assert(scale[0]==1 && anisotropy_in_force()==1);
    benchmark_shadow_size=benchmark_anisotropy=0;
    assert(anisotropy_in_force()==8);
}
''')
    subprocess.run([str(binary)], check=True)


def test_stereo_lod_and_hud_resolution_refresh_without_startup(tmp_path):
    text = (ROOT / "port/linux/game/stereo.c").read_text()
    actual = function(text, "float halo_stereo_lod_scale(") + "\n" + function(text, "float halo_stereo_hud_resolution(")
    binary = compile_probe(tmp_path, r'''
#include <assert.h>
#include <math.h>
#include <string.h>
enum { HALO_STEREO_HEAD=1, HALO_STEREO_SIDE_BY_SIDE=2 };
#define LOD_SCALE_MIN 0.5f
#define LOD_SCALE_MAX 4.0f
#define HUD_RESOLUTION_MIN 0.5f
#define HUD_RESOLUTION_MAX 1.0f
static int stereo_mode;
static double lod=1, hud=1;
static double config_real(const char *name) { return !strcmp(name,"display.lod_scale") ? lod : hud; }
''' + actual + r'''
int main(void) {
    assert(halo_stereo_lod_scale()==1 && halo_stereo_hud_resolution()==1);
    stereo_mode=HALO_STEREO_HEAD;
    lod=3; hud=0.75;
    assert(halo_stereo_lod_scale()==3 && halo_stereo_hud_resolution()==0.75);
    lod=20; hud=-1;
    assert(halo_stereo_lod_scale()==4 && halo_stereo_hud_resolution()==0.5);
    stereo_mode=HALO_STEREO_SIDE_BY_SIDE; lod=0.75; hud=1;
    assert(halo_stereo_lod_scale()==0.75 && halo_stereo_hud_resolution()==1);
}
''')
    subprocess.run([str(binary)], check=True)


def test_every_profile_setting_has_explicit_apply_policy(tmp_path):
    import re
    keys = re.findall(r'^PROFILE_(?:BOOLEAN|INTEGER|REAL|STRING)\("([^"]+)"',
                      (ROOT / "port/linux/src/port_profile_keys.h").read_text(), re.M)
    header = (ROOT / "port/linux/src/port_profile_apply.h").read_text()
    classified = re.findall(r'\{"([^"]+)", PORT_PROFILE_', header)
    assert sorted(classified) == sorted(keys)
    binary = compile_probe(tmp_path, '#include <assert.h>\n' + header.replace('#pragma once', '') + r'''
int main(void) {
    assert(!port_profile_apply_allowed("unknown",1,0));
    assert(!port_profile_apply_allowed("display.renderer",1,0));
    assert(port_profile_apply_allowed("display.render_height",3,1));
    assert(port_profile_apply_allowed("display.shadow_map_size",1,1));
    assert(!port_profile_apply_allowed("display.shadow_map_size",3,0));
    assert(!port_profile_apply_allowed("input.aim_loss_grace",1,1));
    assert(port_profile_apply_allowed("input.aim_loss_grace",1,0));
}
''')
    subprocess.run([str(binary)],check=True)
