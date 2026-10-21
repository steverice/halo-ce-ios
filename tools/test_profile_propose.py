import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profile_cost_model as cost
import profile_predict as predict
import profile_propose as propose

SHADOW, ANISO, LOD, LINES = ("display.shadow_map_size", "display.anisotropic_filtering", "display.model_lod",
                             cost.RESOLUTION_KEY)
BASE = {SHADOW: "512", ANISO: "16", LOD: "max", LINES: "1080"}
RANKING = {"handheld": {
    SHADOW: {"order": ["512", "256", "128"], "grade": {"256": "barely", "128": "obvious"}},
    ANISO: {"order": ["16", "8"], "grade": {"8": "invisible"}},
    LOD: {"order": ["max", "auto"], "grade": {"auto": "barely"}},
    "display.foveation": {"order": ["true", "false"], "grade": {"false": "invisible"}}}}
WITH_LINES = {"handheld": dict(RANKING["handheld"], **{
    LINES: {"order": ["1080", "810", "540", "360"], "grade": {"810": "barely", "540": "obvious", "360": "obvious"}}})}
DEVICE = predict.Device("Test", "A14 Bionic", "A14", native_width=2160, native_height=1080, max_refresh_hz=60)


def fit(sim=2.0, render=2.0, gpu_per_mp=4.0, knobs=None):
    return cost.Fit(device="Test", workload="gameplay", baseline=dict(BASE), simulation_ms=sim, render_cpu_ms=render,
                    gpu_fixed_ms=0.0, gpu_ms_per_megapixel=gpu_per_mp, knobs=knobs or {})


GPU_KNOBS = {SHADOW: {"256": (1.0, 0.8), "128": (1.0, 0.5)}, ANISO: {"8": (1.0, 0.9)}, LOD: {"auto": (0.5, 1.0)}}


def run(item, workload="gameplay", device=DEVICE, ranking=RANKING):
    return propose.propose(item, device, workload, dict(BASE), ranking)


def test_gpu_bound_cuts_gpu_knob_first():
    result = run(fit(gpu_per_mp=9.0, knobs=GPU_KNOBS))  # 19.4 ms of GPU against 4 ms of CPU
    assert result.kind == "change" and result.bound == "gpu"
    assert LOD not in result.settings and set(result.settings) & {SHADOW, ANISO}
    assert result.predicted_fps >= 59.9


def test_cpu_bound_never_cuts_gpu_only_knob():
    result = run(fit(sim=6.0, render=20.0, gpu_per_mp=1.0, knobs=GPU_KNOBS))
    assert result.kind == "change" and result.bound == "cpu"
    assert LOD in result.settings and SHADOW not in result.settings and ANISO not in result.settings


def test_prefers_invisible_steps():
    result = run(fit(gpu_per_mp=7.5, knobs=GPU_KNOBS))  # 17.5 ms; the invisible step alone reaches 15.8
    assert result.settings == {ANISO: "8"}


def test_choice_when_only_obvious():
    result = run(fit(gpu_per_mp=14.0, knobs=GPU_KNOBS))  # 30 ms; only the 128 shadow map reaches 60
    assert result.kind == "choice"
    assert [o.hz for o in result.options] == [60, 30]
    assert result.options[0].worst_grade == "obvious" and result.options[1].worst_grade != "obvious"


def test_floor_not_recommended():
    item = fit(gpu_per_mp=200.0, knobs=GPU_KNOBS)
    start = dict(BASE, **{LINES: "540", SHADOW: "128", ANISO: "8", LOD: "auto"})
    result = propose.propose(item, DEVICE, "gameplay", start, WITH_LINES)
    assert result.kind == "not_recommended" and result.settings == {}


def test_never_touches_fixed_keys():
    knobs = dict(GPU_KNOBS, **{"display.foveation": {"false": (1.0, 0.1)}})
    result = run(fit(gpu_per_mp=9.0, knobs=knobs))
    assert not (set(result.settings) & propose.FIXED)
    for item in run(fit(gpu_per_mp=14.0, knobs=knobs)).options:
        assert not (set(item.settings) & propose.FIXED)


def test_resolution_never_below_floor():
    for per_mp in (20.0, 60.0, 400.0):
        result = run(fit(gpu_per_mp=per_mp, knobs=GPU_KNOBS), ranking=WITH_LINES)
        for item in [result, *result.options]:
            assert int(item.settings.get(LINES, "540")) >= propose.FLOOR_LINES


def test_target_hz():
    vision = predict.Device("Apple Vision Pro", "M2", "M2")
    assert propose.target_hz(vision, "gameplay") == 90
    assert propose.target_hz(DEVICE, "cutscene") == 30
    fast = predict.Device("Fast", "M4", "M4", native_width=2160, native_height=1080, max_refresh_hz=120)
    item = fit(gpu_per_mp=3.0)
    assert propose.target_hz(fast, "gameplay", item, dict(BASE), RANKING) == 120
    assert propose.target_hz(fast, "gameplay", fit(gpu_per_mp=9.0, knobs=GPU_KNOBS), dict(BASE), RANKING) == 60


def test_no_change_when_target_met():
    assert run(fit(gpu_per_mp=2.0)).kind == "none"


def test_markdown_table():
    result = run(fit(gpu_per_mp=7.5, knobs=GPU_KNOBS))
    text = propose.markdown(result, DEVICE, dict(BASE))
    assert "| Setting | Current | Proposed | CPU effect | GPU effect |" in text
    assert f"| `{ANISO}` | 16 | 8 | none | -10% |" in text
    assert "FPS before" in text and "after" in text


def test_cpu_bound_ignores_noise_on_gpu_side_knobs():
    noisy = {SHADOW: {"256": (0.8, 1.0)}, ANISO: {"8": (0.9, 1.0)}, LOD: {"auto": (0.5, 1.0)}}
    result = run(fit(sim=6.0, render=20.0, gpu_per_mp=1.0, knobs=noisy))
    assert result.bound == "cpu" and SHADOW not in result.settings and ANISO not in result.settings
    assert LOD in result.settings


def test_lower_target_needs_no_change_when_the_device_already_meets_it():
    # 60 FPS is out of reach (19 ms of CPU) but the current settings fit 30 FPS (33 ms)
    result = run(fit(sim=2.0, render=17.0, gpu_per_mp=1.0, knobs={SHADOW: GPU_KNOBS[SHADOW]}))
    assert result.kind == "change" and result.target_hz == 30 and result.settings == {}


def test_lower_target_still_cuts_to_reach_it():
    # 60 FPS is out of reach and so is 30 FPS at the current settings (about 38 ms of GPU)
    result = run(fit(gpu_per_mp=17.0, knobs=GPU_KNOBS))
    assert result.kind == "change" and result.target_hz == 30 and result.settings
    assert result.predicted_fps >= 29.9


def test_effects_report_resolution_and_clamp_noise():
    item = fit(gpu_per_mp=12.0, knobs={ANISO: {"8": (0.9, 0.85)}})
    start = dict(BASE)
    effects = propose._effects(item, start, {ANISO: "8", LINES: "810"}, DEVICE)
    assert effects[ANISO] == (1.0, 0.85)  # filtering cannot save CPU time
    cpu, gpu = effects[LINES]
    assert cpu == 1.0 and 0.5 < gpu < 0.7  # (810/1080)^2 of the pixels
