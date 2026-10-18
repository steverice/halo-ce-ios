#!/usr/bin/env python3
"""Execute production replay control/camera consumers with game-edge stubs."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"(?m)^(?:static )?[^;\n]+\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise AssertionError(f"Production definition missing: {name}")
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-regression", action="store_true", help="Require the probe to reject removal of the frame-facing guard")
    args = parser.parse_args(argv)
    pieces = [function("source/game/player_queues_new.c", "update_client_local_player_index")]
    for name in ("player_control_apply_replay", "handle_one_player_input", "player_control_permanent_impulse"):
        pieces.append(function("source/game/player_control.c", name))
    pieces.append("#define config_boolean direct_config_boolean\n" + function("port/linux/game/render_interpolation.c", "render_interpolation_direct_camera") + "\n#undef config_boolean")
    body = "\n\n".join(pieces)
    variants = [("production", body, None)]
    if args.verify_regression:
        variants = []
        facing = "if (!replay_facing && !director_inhibited_facing(local_player_index))"
        variants.append(("frame facing", body.replace(facing, "if (!director_inhibited_facing(local_player_index))"), "desired_angles.yaw == 1"))
        zoom = "if (!replay_facing)"
        assert body.count(zoom) == 2
        first, second = body.index(zoom), body.index(zoom, body.index(zoom) + len(zoom))
        for label, offset in (("weapon zoom reset", first), ("ready zoom reset", second)):
            variants.append((label, body[:offset] + "if (1)" + body[offset + len(zoom):], "zoom_level == 2"))
        variants.append(("scope zoom", body.replace("!replay_facing && TEST_FLAG", "TEST_FLAG"), "zoom_level == 2"))
        variants.append(("direct camera", body.replace(" || input_replay_controls_facing(local_player_index)", ""), "== &observer"))
    template = (ROOT / "port/ios/tests/replay_control_probe.c").read_text()
    with tempfile.TemporaryDirectory(prefix="visr-control-") as folder:
        for label, variant, expected_failure in variants:
            if args.verify_regression:
                assert variant != body, label
            path = Path(folder) / "probe.c"
            path.write_text(template.replace("/* PRODUCTION_FUNCTIONS */", variant))
            output = Path(folder) / "probe"
            subprocess.run(["xcrun", "clang", "-O2", "-fsanitize=address,undefined", "-I" + str(ROOT / "port/linux/src"),
                            "-I" + str(ROOT / "port/ios/tests"), str(path), "-o", str(output)], check=True)
            result = subprocess.run([str(output)], capture_output=True, text=True)
            if expected_failure:
                assert result.returncode != 0 and expected_failure in result.stderr, (label, result.stderr)
                print(f"replay control consumers: {label} guard mutation rejected")
            else:
                print(result.stdout, end="")
                if result.returncode:
                    raise AssertionError(result.stderr)


def test_replay_control_consumers():
    main([])


def test_auto_record_suite_camera_ownership(tmp_path):
    source = ROOT / "port/ios/tests/input_replay_probe.c"
    def build(output, *extra_flags):
        subprocess.run(["xcrun", "clang", "-O2", "-fsanitize=address,undefined",
                        "-I" + str(ROOT / "port/linux/src"), *extra_flags,
                        str(source), "-o", str(output)], cwd=ROOT, check=True)

    fixed = tmp_path / "input-replay-probe"
    build(fixed)
    result = subprocess.run([str(fixed), "--auto-record-suite-camera"], cwd=ROOT,
                            capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stderr)
    assert "auto-record suite camera ownership: PASS" in result.stdout

    omitted = tmp_path / "input-replay-probe-without-suspension"
    build(omitted, "-DREPLAY_PROBE_OMIT_SUITE_SUSPEND=1")
    result = subprocess.run([str(omitted), "--auto-record-suite-camera-baseline"], cwd=ROOT,
                            capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stderr)
    assert "RED reproduced: auto recording owns the action path; camera=0 control=0" in result.stderr

    suite_frame = function("port/linux/src/benchmark_suite.c", "benchmark_suite_frame")
    next_run = function("port/linux/src/benchmark_suite.c", "next_run")
    assert suite_frame.index("input_replay_suite_suspend()") < suite_frame.index("next_run();")
    assert "input_replay_begin(" in next_run


def test_benchmark_suite_suspends_before_recorded_scene(tmp_path):
    output = tmp_path / "benchmark-startup-probe"
    subprocess.run(["xcrun", "clang", "-O2", "-fsanitize=address,undefined",
                    "-I" + str(ROOT / "port/linux/src"), "-iquote", str(ROOT / "port/linux/include"),
                    str(ROOT / "port/ios/tests/benchmark_startup_probe.c"), "-o", str(output)],
                   cwd=ROOT, check=True)
    result = subprocess.run([str(output)], cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stderr)
    assert "benchmark startup: PASS" in result.stdout


if __name__ == "__main__":
    main()
