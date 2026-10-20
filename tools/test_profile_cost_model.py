import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profile_cost_model as model


def run(height=1080, gpu=10.0, settings=None, sim=5.0, render=4.0, thermal=0, basis=model.BASIS, index=0,
        report="r1", device="iPad0,0", workload="gameplay"):
    base = {"display.shadow_map_size": "512", "display.render_lines": str(height)}
    base.update(settings or {})
    return model.Run(report=report, index=index, device=device, chip="A0", workload=workload, settings=base,
                     render_width=height * 2, render_height=height, simulation_cpu_ms=sim, render_cpu_ms=render,
                     gpu_ms=gpu, thermal=thermal, timing_basis=basis)


def megapixels(height):
    return height * 2 * height / 1e6


def test_fit_gpu_fixed_plus_pixels():
    runs = [run(h, 5 + 2 * megapixels(h), index=i) for i, h in enumerate((540, 810, 1080))]
    item = model.fit(runs)[0]
    assert abs(item.gpu_fixed_ms - 5) < 0.05
    assert abs(item.gpu_ms_per_megapixel - 2) < 0.02


def test_knob_factor():
    runs = [run(gpu=10, index=0), run(gpu=10, index=1), run(gpu=9, settings={"display.shadow_map_size": "256"}, index=2)]
    item = model.fit(runs)[0]
    assert abs(item.knobs["display.shadow_map_size"]["256"][1] - 0.9) < 1e-9
    assert item.baseline["display.shadow_map_size"] == "512"
    prediction = model.predict(item, {"display.shadow_map_size": "256"}, 2160, 1080)
    assert abs(prediction.gpu_ms - 9) < 1e-9


def test_fit_skips_zero_gpu():
    runs = [run(540, 0.0, index=0), run(1080, 0.0, index=1)]
    item = model.fit(runs)[0]
    assert item.excluded["gpu_zero"] == 2
    assert item.gpu_fixed_ms == 0 and item.gpu_ms_per_megapixel == 0
    assert item.simulation_ms == 5.0  # the CPU side still fits


def test_fit_excludes_thermal_and_basis():
    runs = [run(index=0), run(index=1, thermal=2), run(index=2, thermal=3),
            run(index=3, basis="submission_timing"), run(index=4, basis="")]
    item = model.fit(runs)[0]
    assert item.excluded == {"thermal": 2, "timing_basis": 2}
    assert item.gpu_ms_per_megapixel > 0


def test_frame_ms_tick_share():
    prediction = model.Prediction(simulation_tick_ms=6, render_cpu_ms=4, gpu_ms=9)
    assert prediction.frame_ms(60) == 9
    assert prediction.frame_ms(30) == 10


def test_runs_csv_roundtrip_dedup(tmp_path):
    path = tmp_path / "runs.csv"
    first = [run(index=0), run(index=1, gpu=7.5, settings={"display.shadow_map_size": "256"})]
    model.write_runs(path, first)
    model.write_runs(path, [run(index=1, gpu=8.0, settings={"display.shadow_map_size": "256"}), run(index=2)])
    loaded = model.read_runs(path)
    assert len(loaded) == 3
    assert [r.gpu_ms for r in loaded if r.index == 1] == [8.0]
    assert loaded[0].settings == first[0].settings


def test_model_toml_roundtrip(tmp_path):
    runs = [run(h, 5 + 2 * megapixels(h), index=i) for i, h in enumerate((540, 1080))]
    runs.append(run(1080, 9, settings={"display.shadow_map_size": "256"}, index=9))
    fits = model.fit(runs)
    model.write_model(tmp_path / "m.toml", fits)
    assert model.read_model(tmp_path / "m.toml") == fits


def test_runs_from_report():
    report = {"code": "ABC", "device": {"model": "iPad0,0", "chip": "A0"}, "runs": [{
        "index": 3, "scene": "gameplay", "status": "complete", "settings": {"shadow_map_size": 512, "render_lines": 540},
        "render_size": [960, 540], "cpu_ms": {"mean": 9}, "gpu_ms": {"mean": 6}, "simulation_cpu_ms": {"mean": 4},
        "render_cpu_ms": {"mean": 5}, "thermal": {"worst": 0}, "timing_basis": model.BASIS}]}
    [item] = model.runs_from_report(report, "r")
    assert (item.simulation_cpu_ms, item.render_cpu_ms, item.gpu_ms, item.render_height) == (4, 5, 6, 540)
    assert item.settings["display.shadow_map_size"] == "512"
