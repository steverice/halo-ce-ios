import json
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profile_cost_model as cost
import profile_pr as pr
import profile_predict as predict

SHADOW, ANISO, LOD, LINES = ("display.shadow_map_size", "display.anisotropic_filtering", "display.model_lod", cost.RESOLUTION_KEY)
BASE = {SHADOW: "512", ANISO: "16", LOD: "max", LINES: "1080"}
RANKING = {"handheld": {
    SHADOW: {"order": ["512", "256", "128"], "grade": {"256": "barely", "128": "obvious"}},
    ANISO: {"order": ["16", "8"], "grade": {"8": "invisible"}}}}
DEVICE = predict.Device("Test Pad", "A14 Bionic", "A14", native_width=2160, native_height=1080, max_refresh_hz=60,
                        identifiers=("iPadTest1,1",))
PROFILE = """schema = 1
name = "Test Pad"
kind = "model"

[match]
platform = "ios"
models = ["iPadTest1,1"]

[display]
shadow_map_size = 512
"""
GPU_KNOBS = {SHADOW: {"256": (1.0, 0.8), "128": (1.0, 0.5)}, ANISO: {"8": (1.0, 0.9)}}


def fit():
    return cost.Fit(device="iPadTest1,1", workload="gameplay", baseline=dict(BASE), simulation_ms=2.0, render_cpu_ms=2.0,
                    gpu_fixed_ms=0.0, gpu_ms_per_megapixel=7.5, knobs=GPU_KNOBS)


class Runner:
    def __init__(self, check_exit=0):
        self.calls = []
        self.check_exit = check_exit

    def __call__(self, *args, **options):
        args = [str(a) for a in args]
        self.calls.append(args)
        if any(a.endswith("profile_check.py") for a in args):
            return SimpleNamespace(returncode=self.check_exit)
        if args[0].endswith("run-job.sh"):
            folder = Path(args[args.index("--fetch") + 1].partition("=")[2])
            for name in ("before", "after"):
                (folder / name).mkdir(parents=True, exist_ok=True)
                (folder / name / "shot-00600.png").write_bytes(b"png")
        if any(a.endswith("data_pack.py") for a in args):
            Path(args[args.index("--output") + 1]).write_text("{}")
        return SimpleNamespace(returncode=0)


def setup(tmp_path, monkeypatch, key=None):
    profiles = tmp_path / "profiles"
    profiles.mkdir()
    (profiles / "test-pad.toml").write_text(PROFILE)
    report = tmp_path / "run" / "report.json"
    report.parent.mkdir()
    report.write_text(json.dumps({"device": {"platform": "ios"}, "runs": []}))
    monkeypatch.setattr(pr, "PROFILES", profiles)
    monkeypatch.setattr(pr, "fits_from", lambda *_: [fit()])
    monkeypatch.setattr(pr.predict, "read_devices", lambda: [DEVICE])
    monkeypatch.setattr(pr.propose, "read_ranking", lambda: RANKING)
    monkeypatch.setattr(pr.check, "profile_keys", lambda: {SHADOW: ("integer", 128, 1024, None), ANISO: ("integer", 1, 16, None)})
    options = pr.argparse.Namespace(model="Test Pad", report=str(report), dry_run=True, out=str(tmp_path / "out"),
                                    workload="gameplay", output_class="handheld", signing_key=key, worktree=str(tmp_path),
                                    map="a10", recording=None, frames=600, exit_after=20.0, maps=None, app=None, host=None)
    return options


def test_dry_run_writes_profile_and_body(tmp_path, monkeypatch):
    options = setup(tmp_path, monkeypatch)
    runner = Runner()
    assert pr.build(options, runner) == 0
    profile = (tmp_path / "out/profiles/test-pad.toml").read_text()
    assert "anisotropic_filtering = 8" in profile and '[evidence."display.anisotropic_filtering"]' in profile
    assert 'basis = "measured"' in profile
    assert (tmp_path / "out/body.md").is_file()
    assert not any(call[0] == "gh" for call in runner.calls)


def test_check_failure_stops_before_pr(tmp_path, monkeypatch):
    options = setup(tmp_path, monkeypatch)
    options.dry_run = False
    options.signing_key = "key"
    runner = Runner(check_exit=1)
    try:
        pr.build(options, runner)
    except pr.Failure as error:
        assert "profile_check" in str(error)
    else:
        raise AssertionError("expected a failure")
    assert not any(call[0] in ("gh", "git") or call[0].endswith("run-job.sh") for call in runner.calls)


def test_body_has_table_and_two_images(tmp_path, monkeypatch):
    options = setup(tmp_path, monkeypatch)
    pr.build(options, Runner())
    text = (tmp_path / "out/body.md").read_text()
    assert "| Setting | Current | Proposed |" in text and "`display.anisotropic_filtering`" in text
    assert text.count("![") == 2
    assert "Confidence: measured" in text
    assert "/Users/" not in text


def test_set_value_adds_and_replaces():
    text = pr.set_value(PROFILE, "display", "shadow_map_size", "256")
    assert "shadow_map_size = 256" in text and text.count("shadow_map_size") == 1
    text = pr.set_value(PROFILE, "input", "aim_loss_grace", "0.5")
    assert "[input]\naim_loss_grace = 0.5" in text


def test_frame_rate_target_only_opens_nothing(tmp_path, monkeypatch):
    options = setup(tmp_path, monkeypatch)
    monkeypatch.setattr(pr.propose, "propose", lambda *a, **k: pr.propose.Proposal("change", {}, 30.0, "cpu", fps_before=40.0, target_hz=30))
    runner = Runner()
    assert pr.build(options, runner) == 0
    assert runner.calls == [] and not (tmp_path / "out/body.md").exists()
