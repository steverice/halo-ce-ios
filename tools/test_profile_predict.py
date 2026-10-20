import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profile_cost_model as cost
import profile_predict as predict

SPEEDS = {f: {"gpu_speed": s, "cpu_speed": s, "basis": "assumed"}
          for f, s in (("A9X", 2.0), ("A12", 4.0), ("A13", 5.0), ("A14", 8.0))}


def fit(device, chip, gpu=10.0):
    return cost.Fit(device=device, workload="gameplay", baseline={}, simulation_ms=4.0, render_cpu_ms=3.0,
                    gpu_fixed_ms=2.0, gpu_ms_per_megapixel=gpu, chip=chip)


def device(model, chip, cores, identifiers=()):
    return predict.Device(model, chip, predict.family_of(chip), gpu_cores=cores, identifiers=identifiers or (model,))


def test_measured_device_uses_own_fit():
    own = fit("iPad1,1", "A14")
    anchor, scale, confidence = predict.anchor_for(device("iPad", "A14", 4, ("iPad1,1",)), [own, fit("x", "A9X")], speeds=SPEEDS)
    assert anchor is own and scale == (1.0, 1.0) and confidence == "measured"


def test_same_family_inferred():
    devices = [device("a", "A14", 4), device("b", "A14", 5)]
    item, scale, confidence = predict.anchor_for(devices[1], [fit("a", "A14")], devices, SPEEDS)
    assert confidence == "inferred" and item.device == "a"


def test_unowned_family_low_confidence():
    devices = [device("a14", "A14", 4), device("a9x", "A9X", 12), device("a12", "A12", 4)]
    fits = [fit("a14", "A14"), fit("a9x", "A9X")]
    result = predict.predicted_fit(devices[2], fits, devices, SPEEDS)
    assert result.confidence == "low" and result.device == "a12"
    assert predict.anchor_for(devices[2], fits, devices, SPEEDS)[0].device == "a14"  # nearest generation


def test_scale_by_gpu_cores():
    devices = [device("five", "A13", 5), device("four", "A13", 4)]
    _, (gpu, cpu), _ = predict.anchor_for(devices[1], [fit("five", "A13")], devices, SPEEDS)
    assert abs(gpu - 1.25) < 1e-9 and cpu == 1.0
    result = predict.predicted_fit(devices[1], [fit("five", "A13", gpu=8.0)], devices, SPEEDS)
    assert abs(result.gpu_ms_per_megapixel - 10.0) < 1e-9


def test_fit_chip_speeds_needs_two_families():
    devices = [device("a", "A12", 4), device("b", "A14", 4)]
    assert predict.fit_chip_speeds([fit("a", "A12")], devices, SPEEDS)["A12"]["basis"] == "assumed"
    refit = predict.fit_chip_speeds([fit("a", "A12", 10.0), fit("b", "A14", 5.0)], devices, SPEEDS)
    assert refit["A14"]["basis"] == "fit" and refit["A14"]["gpu_speed"] > refit["A12"]["gpu_speed"]
