"""Execute window events and the opt-in marker against the production suite."""
from pathlib import Path
import subprocess

import settings_schema

ROOT = Path(__file__).resolve().parents[1]


def test_review_settings_default_off():
    for name in ('debug.benchmark_window_marker', 'debug.benchmark_review_take'):
        setting = settings_schema.config_settings()[name]
        assert setting['default'] is False
        assert setting['type'] == 'boolean'


def test_window_events_and_marker(tmp_path):
    source = tmp_path / 'marker.c'
    source.write_text(r'''
#define WINDOW_MARKER_PROBE 1
#define main startup_probe_main
#include "benchmark_startup_probe.c"
#undef main
static int marker_enabled, review_enabled;
int config_boolean(const char *name) {
    if (!strcmp(name, "debug.benchmark_review_take")) return review_enabled;
    assert(!strcmp(name, "debug.benchmark_window_marker"));
    return marker_enabled;
}
int main(void) {
    char text[96];
    reset();
    strcpy(suite.scene, "campaign-probe");
    strcpy(suite.recording, "probe");
    suite.state = suite_measuring; suite.seconds = 120; suite.opened_ticks = 300;
    timeline_value = 300;
    benchmark_graphics_window(1, 1280, 720);
    CHECK(!strcmp(last_log, "benchmark suite: measured start scene=campaign-probe tick=300"));
    strcpy(last_log, "unchanged");
    benchmark_graphics_window(1, 1280, 720);
    CHECK(!strcmp(last_log, "unchanged"));
    CHECK(!benchmark_suite_window_marker(text, sizeof(text)) && !text[0]);
    marker_enabled = 1; timeline_value = 1410;
    CHECK(benchmark_suite_window_marker(text, sizeof(text)) && !strcmp(text, "MEASURING 0:37 / 2:00"));
    suite.offscreen = 1;
    CHECK(!benchmark_suite_window_marker(text, sizeof(text)) && !text[0]);
    suite.presented_now = 1;
    CHECK(benchmark_suite_window_marker(text, sizeof(text)));
    suite.offscreen = suite.presented_now = 0;
    timeline_value = 3900;
    benchmark_graphics_window(0, 1280, 720);
    CHECK(!strcmp(last_log, "benchmark suite: measured end scene=campaign-probe tick=3900"));
    CHECK(!benchmark_suite_window_marker(text, sizeof(text)));
    strcpy(last_log, "unchanged");
    benchmark_graphics_window(0, 1280, 720);
    CHECK(!strcmp(last_log, "unchanged"));
    suite.state = suite_lead; suite.lead = 135; timeline_value = 3900;
    CHECK(benchmark_suite_window_marker(text, sizeof(text)) && !strcmp(text, "window starts in 5 s"));
    timeline_value = 3000;
    CHECK(!benchmark_suite_window_marker(text, sizeof(text)));
    suite.mp_scene = 1; suite.state = suite_measuring; suite.opened_ticks = 90;
    suite.mp_last_tick = 1200; suite.recording[0] = 0;
    strcpy(suite.scene, "multiplayer-probe");
    benchmark_graphics_window(1, 1280, 720);
    CHECK(!strcmp(last_log, "benchmark suite: measured start scene=multiplayer-probe tick=1200"));
    CHECK(benchmark_suite_window_marker(text, sizeof(text)) && !strcmp(text, "MEASURING 0:37 / 2:00"));
    benchmark_graphics_window(0, 1280, 720);
    suite.state = suite_off;
    CHECK(!benchmark_suite_window_marker(text, sizeof(text)));
    reset();
    strcpy(suite.scene, "tail-probe"); strcpy(suite.recording, "probe");
    suite.state = suite_measuring; suite.seconds = 10; suite.opened_ticks = 0;
    timeline_value = 300; review_enabled = 1;
    benchmark_graphics_window(1, 1280, 720);
    benchmark_suite_frame(1280, 720);
    CHECK(suite.state == suite_review_tail && !graphics_window_open && run_ends == 0);
    CHECK(!benchmark_suite_window_marker(text, sizeof(text)));
    timeline_value = 600; benchmark_suite_frame(1280, 720);
    CHECK(suite.state == suite_review_tail && run_ends == 0);
    timeline_value = 9000; benchmark_suite_frame(1280, 720);
    CHECK(suite.state == suite_review_tail && run_ends == 0);
    replay_ended = 1; benchmark_suite_frame(1280, 720);
    CHECK(run_ends == 1 && !strcmp(last_end_status, "ok"));
    return failures != 0;
}
''')
    binary = tmp_path / 'marker'
    subprocess.run(['cc', '-Werror=implicit-function-declaration', '-I', str(ROOT / 'port/ios/tests'),
                    '-I', str(ROOT / 'port/linux/src'), '-iquote', str(ROOT / 'port/linux/include'),
                    str(source), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_full_take_review_omits_pause_menu_input(tmp_path):
    source = tmp_path / 'pause.c'
    source.write_text(r'''
#define main replay_probe_main
#include "input_replay_probe.c"
#undef main
int main(void) {
    static struct recorded_state states[2];
    XINPUT_GAMEPAD pad = {0};
    memset(&replay, 0, sizeof(replay));
    replay.configured = replay.suite = 1;
    strcpy(replay.replay_path, "probe");
    replay.states = states; replay.state_count = 2;
    states[0].pad.wButtons = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK | 1;
    states[1].ticks = 100;
    clock_ready = 1; clock_tick = 0;
    input_replay_filter(&pad);
    assert(pad.wButtons == (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK | 1));
    review_take = 1; clock_tick = 1;
    input_replay_filter(&pad);
    assert(pad.wButtons == (XINPUT_GAMEPAD_BACK | 1));
    replay.suite = 0;
    input_replay_filter(&pad);
    assert(pad.wButtons == (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK | 1));
    return 0;
}
''')
    binary = tmp_path / 'pause'
    subprocess.run(['cc', '-I', str(ROOT / 'port/ios/tests'), '-I', str(ROOT / 'port/linux/src'),
                    str(source), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
