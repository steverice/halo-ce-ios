from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).parent / 'perf_compare'))
import device_log  # noqa: E402
import device_plan  # noqa: E402
import windows  # noqa: E402

W = windows.load()


def test_opening_plan_has_the_level_and_a_deathless_player():
    plan = device_plan.plan('c10')
    assert plan['init'] == ['game_difficulty_set normal', 'map_name c10', '(set cheat_deathless_player true)']
    assert plan['debug']['frame_log'] == '"on"' and plan['debug']['gpu_stats'] == 'true'
    assert 'network_test' not in plan['debug'] and plan['exit_after'] == 105.0


def test_battle_plan_hosts_the_level_and_holds_the_commands_until_it_runs():
    plan = device_plan.plan('a50-hangar')
    assert plan['debug']['network_test'] == "'host:levels\\a50\\a50:coop'"
    assert plan['debug']['init_after'] == '25.0' and plan['exit_after'] == 130.0
    assert plan['init'][0] == '(set cheat_deathless_player true)' and plan['init'][1:] == W['a50-hangar']['commands']


def test_cutscene_and_bot_plans():
    cut = device_plan.plan('x70-bridge')
    assert cut['init'][-1] == '(begin (x70_bridge) (print "XPDONE"))' and cut['exit_after'] > 25 + 126
    bot = device_plan.plan('b30-bot')
    assert bot['debug']['test_input'] == '"bot:1"' and '(sleep 1800)' in bot['init'][-1]
    with pytest.raises(ValueError):
        device_plan.plan('nope')


def stderr(marker_at=1200):
    lines = ['halo-linux: scene: menu (x)', 'halo-linux: frame 0: 200 draws']
    for frame in range(600, 3601, 600):
        lines.append(f'halo-linux: frame {frame}: 217 draws, 3 immediate')
        if frame == 600:
            lines.append('halo-linux: scene: gameplay')
        if frame == marker_at:
            lines.append('halo-linux: init.txt: running its commands (debug.init_after)')
        lines.append(f'halo-linux: Metal: frames shown for 1/2/3/4/5+ refreshes at 60.0 Hz: 594/3/0/0/0 (pacing off); '
                     f'frame CPU {frame / 100:.1f} ms, GPU 7.0 ms (medians; 95th 9.0, 10.0)')
    return '\n'.join(lines)


def frame_log():
    # 300 frames per 5 s at 60 Hz; late frames grow late in the run
    rows = ['session start=2026-10-27T13:28:11-0700 build=1 version=0.5.1']
    for i in range(14):
        late = 30 if i >= 8 else 0
        rows.append(f'frames time={5 * (i + 1) - 4.1} scene={"menu" if i == 0 else "gameplay"} level=d40 frames=300 fps=60.0 '
                    f'mean_ms=16.7 p95_ms=17.7 p99_ms={18.0 if not late else 33.0} late={late} gaps=0 size=2360x1640 cap=60 hz=60.0 '
                    f'footprint_mb=654 peak_mb=824 thermal=nominal')
    return '\n'.join(rows)


def test_battle_window_aligns_on_the_init_marker_by_frame_number():
    cols = device_log.window_columns(W['b40-exta'], stderr(marker_at=1200), frame_log())
    # marker at frame 1200; warm-up 15 s = 900 frames; the sample would run to frame 5700 but the log ends at 4200
    assert cols['seconds'] == 35.0 and 'shorter than expected' in cols['note'] and cols['resolution'] == '2360×1640'
    assert cols['cap_fps'] == '60' and cols['display_hz'] == '60'
    assert float(cols['fps_avg']) == pytest.approx(1000 / 16.7, abs=0.01)
    assert float(cols['on_time_pct']) < 100  # the late frames after the warm-up are inside the window
    assert float(cols['cpu_ms_p50']) > 20  # renderer lines from frame 2400 on (24 ms and up)
    assert float(cols['gpu_ms_p50']) == 7.0


def test_opening_window_counts_from_the_first_level_scene():
    cols = device_log.window_columns(W['c10'], stderr(), frame_log())
    assert float(cols['fps_1pct_low']) == pytest.approx(1000 / 18.0, abs=0.5) or float(cols['fps_1pct_low']) < 55
    assert 'cpu_ms_p50' in cols


def test_logs_that_do_not_reach_the_window_say_so():
    assert device_log.window_columns(W['b40-exta'], 'halo-linux: scene: menu\n', frame_log())['note']
    short = '\n'.join(frame_log().splitlines()[:3])
    assert device_log.window_columns(W['b40-exta'], stderr(), short)['note']


def test_level_field_is_ignored():
    entries = device_log.parse_frame_log(frame_log())
    assert entries[3]['scene'] == 'gameplay' and 'level' not in entries[3]


def slow_logs(fps=12, marker_frame=300):
    """A device far below its 60 Hz display: 12 fps, a 5 s frame-log entry holds 60 frames, stderr prints every 60."""
    err = ['halo-linux: scene: gameplay']
    for frame in range(60, 2401, 60):
        err.append(f'halo-linux: frame {frame}: 900 draws')
        if frame == marker_frame:
            err.append('halo-linux: init.txt: running its commands (debug.init_after)')
        if frame % 600 == 0:
            err.append('halo-linux: Metal: frames shown for 1/2/3/4/5+ refreshes at 60.0 Hz: 0/0/0/0/600 (pacing off); '
                       f'frame CPU {frame / 100:.1f} ms, GPU 40.0 ms (medians; 95th 50.0, 60.0)')
    rows = ['session start=2026-10-27T13:28:11-0700 build=1 version=0.6.1']
    for i in range(40):
        rows.append(f'frames time={5.0 * (i + 1)} scene=gameplay level=b40 frames={5 * fps} fps={fps}.0 mean_ms={1000 / fps:.1f} '
                    f'p95_ms=90.0 p99_ms=95.0 late={5 * fps} gaps=0 size=1920x1080 cap=0 hz=60.0 footprint_mb=600 peak_mb=700 thermal=nominal')
    return '\n'.join(err), '\n'.join(rows)


def test_battle_window_on_a_device_below_the_display_rate_is_placed_in_seconds():
    err, log = slow_logs()
    cols = device_log.window_columns(W['b40-exta'], err, log)
    # marker at frame 300 (+30 for the print interval) = 27.5 s; the window is 42.5 s to 102.5 s, 60 s long
    assert cols['seconds'] == pytest.approx(60.0, abs=5.0) and 'note' not in cols
    assert float(cols['fps_avg']) == pytest.approx(12.0, abs=0.1)
    # frame-count conversion at 60 Hz would have started 900 frames in and run 3600 frames: past the log's end
    assert cols['frames'] == pytest.approx(60 * 12, abs=60)
    assert cols['cpu_ms_p95'] == '50.0' and cols['gpu_ms_p95'] == '60.0'
    assert float(cols['gpu_ms_p50']) == 40.0
