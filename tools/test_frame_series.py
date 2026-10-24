"""tools/frame_series.py: per-second costs placed by the recorded tick the window opened at."""
import base64
import struct

import pytest

import frame_series


def encode(values):
    return base64.b64encode(struct.pack("<%dH" % len(values), *[int(v * 10) for v in values])).decode()


def ticks_plus(values):
    return base64.b64encode(struct.pack("<%dH" % len(values), *values)).decode()


def report(opened=900, seconds=4.0, frames=8):
    # each frame ran 15 ticks, half a second of the take
    return {"runs": [{"window_opened_tick": opened, "seconds": seconds, "frame_ticks": ticks_plus([16] * frames),
                      "frame_cpu": encode([1.0] * frames), "frame_gpu": encode([5.0] * 4 + [9.0] * 4),
                      "frame_simulation": encode([2.0] * frames), "frame_render": encode([3.0] * frames)}]}


def test_seconds_start_at_the_recorded_tick_the_window_opened_at():
    rows = frame_series.series(report(opened=900))
    assert rows[0][0] == 30 and rows[-1][0] == 33


def test_gpu_and_cpu_are_per_second_means_and_cpu_adds_simulation_and_render():
    rows = frame_series.series(report(opened=0))
    assert [round(row[2], 1) for row in rows] == [5.0, 5.0, 9.0, 9.0]
    assert all(abs(row[1] - 5.0) < 1e-6 for row in rows) and all(abs(row[3] - 1.0) < 1e-6 for row in rows)


def test_a_run_that_did_not_record_its_window_tick_is_refused():
    broken = report()
    del broken["runs"][0]["window_opened_tick"]
    with pytest.raises(ValueError):
        frame_series.series(broken)
    broken["runs"][0]["window_opened_tick"] = -1
    with pytest.raises(ValueError):
        frame_series.series(broken)


def test_a_report_without_the_split_series_still_gives_gpu():
    plain = report()
    del plain["runs"][0]["frame_simulation"], plain["runs"][0]["frame_render"]
    rows = frame_series.series(plain)
    assert rows and rows[0][1] == 0.0 and rows[0][2] == 5.0


def test_frames_are_placed_by_their_ticks_so_frameless_stretches_leave_gaps():
    # three frames of 30 ticks, then a skip of 300 ticks (no frames) is not in the series; the next frame's
    # tick count places it after the frames before it only
    plain = report(opened=0, frames=8)
    plain["runs"][0]["frame_ticks"] = ticks_plus([31, 31, 31, 301, 31, 31, 31, 31])
    rows = frame_series.series(plain)
    assert [row[0] for row in rows] == [0, 1, 2, 3, 13, 14, 15, 16]


def test_game_report_seconds_preserve_unavailable_gpu():
    from test_game_perf_reports import report as game_report
    rows = frame_series.series(game_report())
    assert rows == [(0, 10.0, None, 3.0)]
