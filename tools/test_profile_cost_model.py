import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profile_cost_model as model


def run(height=1080, gpu=10.0, settings=None, sim=5.0, render=4.0, thermal=0, basis=model.BASIS, index=0,
        report="r1", device="iPad0,0", workload="gameplay", render_basis="waits_removed"):
    base = {"display.shadow_map_size": "512", "display.render_lines": str(height)}
    base.update(settings or {})
    return model.Run(report=report, index=index, device=device, chip="A0", workload=workload, settings=base,
                     render_width=height * 2, render_height=height, simulation_cpu_ms=sim, render_cpu_ms=render,
                     gpu_ms=gpu, thermal=thermal, timing_basis=basis, render_cpu_basis=render_basis)


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


def test_serialized_wall_render_cpu_is_not_a_cost():
    # serialized offscreen runs' render CPU counts the visibility waits: only with them taken out, or pipelined
    wall = [run(index=0, render=9.0, render_basis="wall"),
            run(index=1, render=12.0, render_basis="wall", settings={"display.shadow_map_size": "256"})]
    item = model.fit(wall)[0]
    assert item.excluded["render_cpu_serialized"] == 2 and item.excluded["render_cpu_upper_bound"] == 1
    assert "display.shadow_map_size" not in item.knobs or item.knobs["display.shadow_map_size"]["256"][0] == 1.0
    assert item.render_cpu_ms == 9.0
    piped = [run(index=0, render=4.0, basis="offscreen_pipelined", render_basis="wall"),
             run(index=1, render=5.0, basis="offscreen_pipelined", render_basis="wall",
                 settings={"display.shadow_map_size": "256"}),
             run(index=2, render=7.0, render_basis="wall")]
    item = model.fit(piped)[0]
    assert item.excluded == {"render_cpu_serialized": 1}
    assert abs(item.knobs["display.shadow_map_size"]["256"][0] - 1.25) < 1e-9
    assert item.render_cpu_ms == 4.0


def test_runs_from_report_takes_out_gpu_waits():
    report = {"code": "W", "device": {"model": "iPad0,0", "chip": "A0"}, "runs": [
        {"index": 0, "scene": "gameplay", "status": "complete", "settings": {}, "render_size": [960, 540],
         "render_cpu_ms": {"mean": 9.0}, "gpu_wait_ms": {"mean": 5.5}, "timing_basis": "offscreen_uncapped"},
        {"index": 1, "scene": "gameplay", "status": "complete", "settings": {}, "render_size": [960, 540],
         "render_cpu_ms": {"mean": 9.0}, "timing_basis": "offscreen_uncapped"}]}
    first, second = model.runs_from_report(report)
    assert first.render_cpu_ms == 3.5 and first.render_cpu_basis == "waits_removed" and first.render_cpu_valid()
    assert second.render_cpu_ms == 9.0 and second.render_cpu_basis == "wall" and not second.render_cpu_valid()


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


def test_runs_keep_catalog_signed(tmp_path):
    report = {"code": "LOC", "catalog_signed": False, "device": {"model": "Mac0,0"}, "runs": [
        {"index": 0, "scene": "check", "status": "ok", "render_size": [960, 540], "gpu_ms": {"mean": 6},
         "catalog_signed": False, "timing_basis": model.BASIS},
        {"index": 1, "scene": "check", "status": "ok", "render_size": [960, 540], "gpu_ms": {"mean": 6}}]}
    unsigned, inherited = model.runs_from_report(report, "loc")
    assert unsigned.catalog_signed is False and inherited.catalog_signed is False
    assert model.runs_from_report({"runs": [{"index": 0, "status": "ok"}]})[0].catalog_signed is True
    path = tmp_path / "runs.csv"
    model.write_runs(path, [unsigned, run(index=5)])
    assert [r.catalog_signed for r in model.read_runs(path)] == [False, True]
    assert "catalog_signed" in path.read_text().splitlines()[0]


def test_runs_csv_without_catalog_signed_column(tmp_path):
    path = tmp_path / "runs.csv"
    model.write_runs(path, [run(index=0)])
    lines = path.read_text().splitlines()
    path.write_text("\n".join(line.rsplit(",", 1)[0] for line in lines) + "\n")
    assert model.read_runs(path)[0].catalog_signed is True


def test_report_run_of_a_variant_counts_for_its_workload():
    report = {"device": {"model": "iPad0,0", "chip": "A0"}, "runs": [
        {"index": 0, "status": "ok", "scene": "a10-mess-hall-min", "workload": "a10-mess-hall", "render_size": [960, 540],
         "settings": {"render_lines": 540, "sim_experiments": 5, "sim_experiments_mask": 7, "debug.static_light_relink": "on",
                      "display.anti_aliasing": "off", "upscaler_requested": "bilinear"},
         "simulation_cpu_ms": {"mean": 4.0}, "render_cpu_ms": {"mean": 3.0}, "gpu_ms": {"mean": 6.0},
         "timing_basis": "offscreen_uncapped"}]}
    (item,) = model.runs_from_report(report, "r")
    assert item.workload == "a10-mess-hall"
    assert item.settings == {"display.render_lines": "540", "debug.static_light_relink": "on",
                             "display.anti_aliasing": "off"}
