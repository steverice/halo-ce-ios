import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchmark_catalog
import cost_suite as suites
import profile_cost_model as model
import test_benchmark_catalog as fixtures

LADDER = {
    "display.render_lines": ["1080", "810", "540"],
    "display.shadow_map_size": ["512", "256", "128"],
    "display.anti_aliasing": ["smaa", "fxaa", "off"],
    "debug.collision_type_filter": ["off", "on"],
    "debug.static_light_relink": ["off", "on"],
}
BASELINE = {"display.render_lines": "1080", "display.shadow_map_size": "512", "display.anti_aliasing": "off",
            "debug.collision_type_filter": "off", "debug.static_light_relink": "off"}


def case(name, kind="gameplay", **extra):
    return {"id": name, "kind": kind, "order": 0, "map": "b30", "difficulty": "hard", "recording": f"take-{name}",
            "lead": 0, "window": 1, **extra}


WORKLOADS = [case("c-one"), case("c-two")]


def row(workload, settings, height, cpu=5.0, gpu=5.0, device="iPad0,0", thermal=0, basis=model.BASIS):
    return model.Run(report="r", index=0, device=device, chip="A0", workload=workload, settings=settings,
                     render_width=height * 2, render_height=height, simulation_cpu_ms=cpu, render_cpu_ms=0.0,
                     gpu_ms=gpu, thermal=thermal, timing_basis=basis)


def bound_rows(workload, low_ms, high_ms, **extra):
    low = {"display.shadow_map_size": "128", "display.anti_aliasing": "off", "debug.collision_type_filter": "on",
           "debug.static_light_relink": "on"}
    high = {"display.shadow_map_size": "512", "display.anti_aliasing": "smaa", "debug.collision_type_filter": "off",
            "debug.static_light_relink": "off"}
    return [row(workload, low, 540, cpu=low_ms, gpu=low_ms, **extra), row(workload, high, 2160, cpu=high_ms, gpu=high_ms, **extra)]


def test_bounds_suite_has_min_and_max_per_workload():
    suite = suites.bounds_suite(WORKLOADS, LADDER)
    assert [run.id for run in suite.runs] == ["c-one-min", "c-one-max", "c-two-min", "c-two-max"]
    low, high = suite.runs[0].settings, suite.runs[1].settings
    assert low == {"display.render_height": 540, "display.shadow_map_size": 128, "display.anti_aliasing": "off",
                   "debug.collision_type_filter": True, "debug.static_light_relink": True}
    assert high == {"display.render_height": 0, "display.shadow_map_size": 512, "display.anti_aliasing": "smaa",
                    "debug.collision_type_filter": False, "debug.static_light_relink": False}


def test_a_knob_added_to_the_ladder_is_data_only():
    ladder = {**LADDER, "display.mirror_resolution": ["full", "half"]}
    suite = suites.bounds_suite(WORKLOADS, ladder)
    assert suite.runs[0].settings["display.mirror_resolution"] == "half"
    assert suite.runs[1].settings["display.mirror_resolution"] == "full"


def test_ladder_knob_the_catalog_cannot_carry_is_refused():
    with pytest.raises(ValueError, match="not a setting"):
        suites.bounds_suite(WORKLOADS, {"display.future_knob": ["off", "80"]})


def test_ofat_one_knob_per_run():
    knobs = suites.ofat_knobs(LADDER)
    suite = suites.ofat_suite(WORKLOADS[:1], BASELINE, knobs)
    base = suites._settings(BASELINE)
    assert suite.runs[0].id == "c-one-base" and suite.runs[0].settings == base
    singles = suite.runs[1:-1]
    for run in singles:
        assert len([name for name in base if run.settings[name] != base[name]]) == 1
    assert {run.settings["display.render_height"] for run in singles} >= {540, 720}
    # every other value of every other knob appears once, and all the switches on together comes last
    assert len(singles) == 3 + 2 + 2 + 1 + 1 and len({run.id for run in suite.runs}) == len(suite.runs)
    assert suite.runs[-1].id == "c-one-sim-all"
    assert suite.runs[-1].settings["debug.collision_type_filter"] and suite.runs[-1].settings["debug.static_light_relink"]


def test_next_suite_drops_floor_failures():
    runs = bound_rows("c-one", 40.0, 50.0) + bound_rows("c-two", 20.0, 30.0)
    suite = suites.next_suite(runs, "iPad0,0", WORKLOADS, BASELINE, LADDER)
    assert {run.workload for run in suite.runs} == {"c-two"}
    assert suite.not_recommended == ["c-one"]


def test_next_suite_drops_max_passes():
    runs = bound_rows("c-one", 5.0, 10.0) + bound_rows("c-two", 8.0, 30.0)
    suite = suites.next_suite(runs, "iPad0,0", WORKLOADS, BASELINE, LADDER)
    assert {run.workload for run in suite.runs} == {"c-two"}
    assert suite.no_tuning == ["c-one"]


def test_next_suite_none_when_nothing_is_left():
    runs = bound_rows("c-one", 5.0, 10.0) + bound_rows("c-two", 40.0, 50.0)
    assert suites.next_suite(runs, "iPad0,0", WORKLOADS, BASELINE, LADDER) is None


def test_next_suite_ignores_other_devices_thermal_and_basis():
    runs = (bound_rows("c-one", 40.0, 50.0, device="other") + bound_rows("c-one", 40.0, 50.0, thermal=3) +
            bound_rows("c-one", 40.0, 50.0, basis="submission_timing"))
    suite = suites.next_suite(runs, "iPad0,0", WORKLOADS[:1], BASELINE, LADDER)
    assert suite is not None and suite.not_recommended == []  # no usable bounds yet: not judged


def test_chunks_fit_the_catalog_limit_and_repeat_the_workload():
    suite = suites.ofat_suite(WORKLOADS, BASELINE, suites.ofat_knobs(LADDER))
    chunks = suite.chunks(16)
    assert sum(len(chunk.runs) for chunk in chunks) == len(suite.runs)
    for chunk in chunks:
        document = chunk.catalog()["cases"]
        assert len(document) <= 16
        assert {case["variant_of"] for case in document.values() if "variant_of" in case} <= set(document)


def test_written_suite_loads_as_a_catalog(tmp_path):
    source = tmp_path / "source"
    for name, map_name in (("c-one", "m1"), ("c-two", "m2")):
        fixtures._recording(source, f"take-{name}", scene=name, map_name=map_name)
    workloads = [case("c-one", map="m1"), case("c-two", map="m2")]
    suite = suites.bounds_suite(workloads, LADDER)
    mode = suite.write(tmp_path / "out", source)
    assert mode == "cost:c-one-min,c-one-max,c-two-min,c-two-max"
    loaded = {item["id"]: item for item in benchmark_catalog.load_catalog(tmp_path / "out")}
    assert loaded["c-one-min"]["settings"]["display.render_height"] == 540
    assert loaded["c-one-min"]["recording"] == "take-c-one"


def test_select_workloads_by_id_and_heaviest():
    cases = [case("c-a"), case("c-b"), case("c-c"),
             {"id": "mp16-chillout", "kind": "multiplayer"}, {"id": "mp32-bloodgulch", "kind": "multiplayer"},
             {"id": "mp-sidewinder-128", "kind": "multiplayer"}]
    chosen = suites.select_workloads(cases, {"c-a": 3.0, "c-b": 9.0, "c-c": 5.0})
    assert [c["id"] for c in chosen] == ["c-b", "c-a", "mp16-chillout", "mp32-bloodgulch", "mp-sidewinder-128"]
    named = suites.select_workloads(cases, heaviest="c-c", other="c-b")
    assert [c["id"] for c in named][:2] == ["c-c", "c-b"]
    with pytest.raises(ValueError, match="mp16-chillout"):
        suites.select_workloads(cases[:3])


def test_heaviest_by_gpu_skips_unusable_runs():
    runs = [row("c-a", {}, 1080, gpu=9.0), row("c-a", {}, 1080, gpu=0.0), row("c-b", {}, 1080, gpu=30.0, thermal=3)]
    assert suites.heaviest_by_gpu(runs) == {"c-a": 9.0}


def test_gpu_times_from_sheet(tmp_path):
    path = tmp_path / "rows.jsonl"
    rows = [{"scene": "c-a", "kind": "gameplay", "status": "ok", "gpu_ms_mean": 4.0},
            {"scene": "c-a", "kind": "gameplay", "status": "ok", "gpu_ms_mean": 6.0},
            {"scene": "c-b", "kind": "gameplay", "status": "ok", "gpu_ms_mean": 0.0},
            {"scene": "c-c", "kind": "gameplay", "status": "failed", "gpu_ms_mean": 9.0},
            {"scene": "mp", "kind": "multiplayer", "status": "ok", "gpu_ms_mean": 9.0}]
    path.write_text("\n".join(json.dumps(row) for row in rows))
    assert suites.gpu_times_from_sheet(path) == {"c-a": 5.0}


def test_clutter_knobs_are_in_the_bounds_runs():
    ladder = {"display.clutter_distance": ["0", "80", "40", "20"], "debug.clutter_lowest_lod": ["off", "on"]}
    suite = suites.bounds_suite(WORKLOADS[:1], ladder)
    assert suite.runs[0].settings == {"display.clutter_distance": 20.0, "debug.clutter_lowest_lod": True}
    assert suite.runs[1].settings == {"display.clutter_distance": 0.0, "debug.clutter_lowest_lod": False}
