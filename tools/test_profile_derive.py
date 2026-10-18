"""tools/profile_derive.py against made-up reports: the targets (60 in play, 30 in cutscenes, never a 30 gameplay
profile), the 120 note, the pieced-together case, and reports built from one-launch run logs."""
import base64
import gzip
import json
import struct

import pytest

from tools import profile_derive


def run(index, stage, scene, kind, lines, *, gpu, cpu=6.0, hertz=59.94, status="ok", anisotropy=16, shadow=512, lod="max",
        left=900):
    period = 1000.0 / hertz
    interval = -(-max(gpu, cpu) // (period * 0.85)) * period   # whole refreshes under vsync
    return {"index": index, "stage": stage, "scene": scene, "map": scene.split("-")[0], "kind": kind, "difficulty": "normal",
            "recording": None, "note": "", "status": status, "render_size": [lines * 16 // 9, lines],
            "settings": {"render_lines": lines, "anisotropic_filtering": anisotropy, "shadow_map_size": shadow, "model_lod": lod},
            "frames": 3600, "seconds": 3600 * interval / 1000.0, "frames_a_second": 1000.0 / interval, "refresh_hertz": hertz,
            "interval_ms": {"mean": interval, "median": interval, "p95": interval, "p99": interval, "worst": interval},
            "cpu_ms": {"mean": cpu, "median": cpu, "p95": cpu, "p99": cpu, "worst": cpu},
            "gpu_ms": {"mean": gpu, "median": gpu, "p95": gpu, "p99": gpu, "worst": gpu},
            "shown_refreshes": [3600, 0, 0, 0, 0], "on_time_full": 1.0 if interval <= period * 1.05 else 0.0, "on_time_half": 1.0,
            "memory_mb": {"footprint_first": 500, "footprint_last": 520, "footprint_peak": 600, "left_least": left},
            "thermal": {"first": 0, "worst": 0, "last": 0, "changes": [], "change_count": 0}}


def report(runs, *, model="AppleTV14,1", drawable=2160, code="TESTCODE", flags=()):
    return {"format": 1, "code": code, "suite": 1, "mode": "full", "started": "2026-10-13T20:00Z",
            "device": {"model": model, "chip": "", "platform": "tvos", "os": "26.6.0", "os_build": "23M1", "gpu": "Apple A15 GPU",
                       "gpu_family": 8, "memory_mb": 3700, "screen": {"width": 3840, "height": drawable, "maximum_frames_a_second": 60}},
            "app": {"version": "0.1.2", "build": "1", "image": "00"},
            "profile": {"drawable_lines": drawable, "render_lines": 1080, "anisotropic_filtering": 16, "shadow_map_size": 512,
                        "model_lod": "max"}, "flags": list(flags), "runs": runs}


def scenes(stage, lines, gpu, start=0, **more):
    return [run(start, stage, "menu", "menu", lines, gpu=gpu * 0.4, **more),
            run(start + 1, stage, "a10-opening", "cutscene", lines, gpu=gpu * 0.6, **more),
            run(start + 2, stage, "a10-crew", "gameplay", lines, gpu=gpu, **more),
            run(start + 3, stage, "b30-beach", "gameplay", lines, gpu=gpu * 1.2, **more)]


def test_holds_is_the_documented_rule():
    good = run(0, "A", "a10-crew", "gameplay", 1080, gpu=12.0)
    assert profile_derive.holds(good, 60.0) == (True, "")
    assert not profile_derive.holds(run(0, "A", "x", "gameplay", 1080, gpu=14.5), 60.0)[0]      # over 85% of 16.7 ms
    assert profile_derive.holds(run(0, "A", "x", "cutscene", 1080, gpu=27.0), 30.0)[0]           # 30: 28.4 ms allowed
    assert not profile_derive.holds(run(0, "A", "x", "gameplay", 1080, gpu=8.0, left=250), 60.0)[0]
    assert profile_derive.holds(run(0, "A", "x", "gameplay", 1080, gpu=8.0, left=-1), 60.0)[0]   # unknown memory passes
    assert not profile_derive.holds(run(0, "A", "x", "gameplay", 1080, gpu=8.0, status="thermal"), 60.0)[0]
    # at 120 Hz a frame every two refreshes holds 60
    assert profile_derive.holds(run(0, "A", "x", "gameplay", 1080, gpu=12.0, hertz=120.0), 60.0)[0]
    assert not profile_derive.holds(run(0, "A", "x", "gameplay", 1080, gpu=12.0, hertz=120.0), 120.0)[0]


def test_a_confirmed_combination_is_proposed_with_evidence():
    runs = scenes("A", 1080, 6.0) + scenes("E", 1440, 10.0, start=4) + [
        run(8, "B", "a10-crew", "gameplay", 2160, gpu=22.0), run(9, "D", "menu", "menu", 2160, gpu=9.0),
        run(10, "D", "a10-opening", "cutscene", 2160, gpu=14.0)]
    proposal, notes = profile_derive.derive([report(runs)])
    assert 'status = "provisional"' in proposal and 'models = ["AppleTV14,1"]' in proposal
    assert "render_height = 2160" in proposal                         # the menu and cutscene go to 2160
    assert "render_scale = 0.667" in proposal                         # 1440 of 2160 in play
    assert "[scene.menu.display]\nrender_scale = 1" in proposal
    assert "[scene.cutscene.display]\nrender_scale = 1" in proposal   # 14 ms holds 30 at 2160
    assert proposal.count('basis = "measured"') == proposal.count("[evidence.")
    assert "device benchmark report TESTCODE" in proposal
    assert not any("NO SETTINGS" in note for note in notes)


def test_nothing_holding_60_is_said_plainly_and_30_is_never_proposed():
    runs = scenes("A", 1080, 40.0) + [run(4, "B", "a10-crew", "gameplay", 540, gpu=18.0),
                                      run(5, "B", "b30-beach", "gameplay", 540, gpu=21.0)]
    proposal, notes = profile_derive.derive([report(runs, model="AppleTV5,3", drawable=1080)])
    assert 'status = "below target"' in proposal
    assert "render_scale = 0.5" in proposal                            # the cheapest size tried, not a 30 target at 1080
    assert any("NO SETTINGS TRIED HOLD 60" in note for note in notes)
    assert "BELOW TARGET" in proposal
    assert "30 fps" not in proposal.split("[scene.")[0] or "holds 30" not in proposal.split("[scene.")[0]


def test_one_setting_runs_are_pieced_together_and_marked():
    runs = [run(0, "A", "a10-crew", "gameplay", 1080, gpu=20.0), run(1, "A", "b30-beach", "gameplay", 1080, gpu=24.0),
            run(2, "B", "a10-crew", "gameplay", 720, gpu=9.0), run(3, "B", "b30-beach", "gameplay", 720, gpu=10.5),
            run(4, "C", "b30-beach", "gameplay", 720, gpu=9.0, anisotropy=4)]
    proposal, notes = profile_derive.derive([report(runs)])
    assert "render_scale = 0.333" in proposal and "anisotropic_filtering = 16" in proposal
    assert 'status = "provisional"' in proposal


def test_120_is_an_option_not_the_default():
    runs = [run(0, "A", "a10-crew", "gameplay", 1440, gpu=9.0, hertz=120.0),
            run(1, "A", "b30-beach", "gameplay", 1440, gpu=10.0, hertz=120.0),
            run(2, "F", "b30-beach", "gameplay", 1080, gpu=6.0, hertz=120.0)]
    proposal, notes = profile_derive.derive([report(runs, model="iPad13,8", drawable=1440)])
    assert "render_scale = 1\n" in proposal
    assert any(note.startswith("120:") and "option, not the default" in note and "one size lower" in note for note in notes)
    fast = [run(0, "A", "a10-crew", "gameplay", 1440, gpu=4.0, cpu=3.0, hertz=120.0)]
    _, notes = profile_derive.derive([report(fast, model="iPad13,8", drawable=1440)])
    assert any("can be the default" in note for note in notes)


def test_spoiled_runs_are_not_evidence_and_flags_are_reported():
    runs = scenes("A", 1080, 6.0) + [run(4, "B", "a10-crew", "gameplay", 1440, gpu=6.0, status="thermal"),
                                     run(5, "B", "b30-beach", "gameplay", 1440, gpu=7.0)]
    proposal, notes = profile_derive.derive([report(runs, flags=["throttled"])])
    assert "render_scale = 0.5" in proposal                           # 1440 has a spoiled run: 1080 stands
    assert any("throttled" in note for note in notes)


def test_synthetic_multiplayer_does_not_change_profile_proposals():
    solo = scenes("A", 1080, 6.0)
    multiplayer = run(4, "MP", "mp-chillout-4", "multiplayer_synthetic", 2160, gpu=1.0)
    assert profile_derive.derive([report(solo)]) == profile_derive.derive([report(solo + [multiplayer])])


def test_reports_for_two_models_are_refused():
    with pytest.raises(ValueError):
        profile_derive.derive([report(scenes("A", 1080, 6.0)), report(scenes("A", 1080, 6.0), model="AppleTV6,2")])


def test_reading_reports_and_frames(tmp_path):
    one = report(scenes("A", 1080, 6.0))
    one["runs"][0]["frame_interval"] = base64.b64encode(struct.pack("<3H", 167, 167, 333)).decode()
    plain, packed = tmp_path / "report.json", tmp_path / "report.json.gz"
    plain.write_text(json.dumps(one))
    packed.write_bytes(gzip.compress(json.dumps(one).encode()))
    assert profile_derive.load_report(plain) == profile_derive.load_report(packed)
    assert profile_derive.frame_times(one["runs"][0], "frame_interval") == [16.7, 16.7, 33.3]
    assert profile_derive.frame_times(one["runs"][1], "frame_interval") == []
    other = tmp_path / "old.json"
    other.write_text(json.dumps(dict(one, format=7)))
    with pytest.raises(ValueError):
        profile_derive.load_report(other)
    table = profile_derive.run_table([one])
    assert table.count("\n") == 2 + 4 and "| a10-crew |" in table


def test_proposals_are_never_written_into_profiles(tmp_path):
    source = tmp_path / "report.json"
    source.write_text(json.dumps(report(scenes("A", 1080, 6.0))))
    (tmp_path / "profiles").mkdir()
    with pytest.raises(SystemExit):
        profile_derive.main(["derive", str(source), "--out", str(tmp_path / "profiles" / "x.toml")])
    assert profile_derive.main(["derive", str(source), "--out", str(tmp_path / "proposal.toml")]) == 0
    assert (tmp_path / "proposal.toml").read_text().startswith("# Proposed by tools/profile_derive.py")


def test_a_report_from_run_logs(tmp_path):
    folder = tmp_path / "b30-aniso-4"
    folder.mkdir()
    window = ("halo-linux: Metal: frames shown for 1/2/3/4/5+ refreshes at 59.9 Hz: {a}/{b}/0/0/0 (pacing off); "
              "frame CPU {c} ms, GPU {g} ms (medians; 95th {c2}, {g2})\n")
    (folder / "stderr.log").write_text(window.format(a=600, b=0, c=5.0, g=7.0, c2=6.0, g2=8.0) * 2
                                       + window.format(a=100, b=500, c=14.0, g=21.0, c2=16.0, g2=24.0) * 2)
    (folder / "ios-runtime.log").write_text("tvOS: render height 540 on this GPU\nmemory: footprint 540 MB, peak 660 MB, "
                                            "904 MB left before the system's limit\n")
    (folder / "config.toml").write_text("[display]\nanisotropic_filtering = 4\nshadow_map_size = 512\nmodel_lod = \"max\"\n")
    built = profile_derive.report_from_runs([folder], "AppleTV5,3", "tvos", 1080, "RUNLOGS1")
    one = built["runs"][0]
    # the second half of the windows is the scene: 100 on time, 500 held for two refreshes
    assert one["status"] == "ok" and one["frames"] == 1200 and one["shown_refreshes"][:2] == [200, 1000]
    assert one["settings"] == {"render_lines": 540, "anisotropic_filtering": 4, "shadow_map_size": 512, "model_lod": "max"}
    assert one["gpu_ms"]["p95"] == 24.0 and one["memory_mb"]["left_least"] == 904
    assert abs(one["interval_ms"]["p99"] - 2000.0 / 59.9) < 1e-6
    proposal, notes = profile_derive.derive([built])
    assert 'status = "below target"' in proposal
