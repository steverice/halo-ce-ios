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


def test_replay_environment(tmp_path):
    output = tmp_path / "replay-environment"
    consumer = r"""
    typedef int boolean;
    #define NUMBEROF(a) (sizeof(a) / sizeof((a)[0]))
    #define _stricmp strcasecmp
    struct unit_datum { long definition_index; } environment_unit;
    static struct unit_datum *unit_get(long index) { (void)index; return &environment_unit; }
    static const char *tag_get_name(long index) { (void)index; return "cyborg"; }
    static const char *tag_name_strip_path(const char *name) { return name; }
    unsigned long config_changes(void) { return environment_config_changes; }
    static const char *enhanced_animation_units[] = {"cyborg", "cyborg_mp"};
    """ + function("source/units/units.c", "unit_animation_enhanced")
    source = tmp_path / "replay-environment.c"
    source.write_text((ROOT / "port/ios/tests/input_replay_probe.c").read_text().replace(
        "/* PRODUCTION_ENHANCED_CONSUMER */", consumer))
    subprocess.run(["xcrun", "clang", "-O2", "-fsanitize=address,undefined",
                    "-DREPLAY_ENVIRONMENT_CONSUMER=1", "-I" + str(ROOT / "port/linux/src"),
                    str(source), "-o", str(output)], cwd=ROOT, check=True)
    result = subprocess.run([str(output), "--replay-environment"], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "replay environment: PASS" in result.stdout
    result = subprocess.run([str(output)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "input replay mouth mode: passed" in result.stdout
    game_tick = function("source/game/game.c", "game_tick")
    assert game_tick.index("input_replay_tick_begin();") < game_tick.index("units_update();")
    assert game_tick.index("input_replay_tick_begin();") < game_tick.index("ai_update();")
    assert game_tick.index("objects_update();") < game_tick.index("input_replay_tick_end();")
    # The same source probe must reject losing either first/final-tick ownership or cache bypass.
    implementation = (ROOT / "port/linux/src/input_replay.c").read_text()
    for name, mutated_implementation, mutated_consumer in [
        ("first-tick", implementation.replace("replay.environment_owned = TRUE;", "replay.environment_owned = FALSE;"), consumer),
        ("final-tick", implementation.replace("replay.environment_final_tick = replay.environment_owned;",
                                              "replay.environment_final_tick = FALSE;"), consumer),
        ("cached-config", implementation, consumer.replace("input_replay_enhanced_animations(enhanced)", "enhanced")),
    ]:
        variant = tmp_path / (name + ".c")
        variant.write_text(mutated_implementation.replace('"../../runtime/replay_environment.h"',
            '"' + str(ROOT / "port/runtime/replay_environment.h") + '"'))
        source.write_text((ROOT / "port/ios/tests/input_replay_probe.c").read_text().replace(
            "/* PRODUCTION_ENHANCED_CONSUMER */", mutated_consumer).replace(
            '"input_replay.c"', '"' + str(variant) + '"'))
        subprocess.run(["xcrun", "clang", "-O2", "-fsanitize=address,undefined",
                        "-DREPLAY_ENVIRONMENT_CONSUMER=1", "-I" + str(ROOT / "port/linux/src"),
                        str(source), "-o", str(output)], cwd=ROOT, check=True)
        result = subprocess.run([str(output), "--replay-environment"], capture_output=True, text=True)
        assert result.returncode != 0 and "expected" in result.stderr, (name, result.stdout, result.stderr)


def test_pinned_replay_environment_report(tmp_path):
    folder = tmp_path / "benchmark" / "recordings"
    folder.mkdir(parents=True)
    source = tmp_path / "report.m"
    source.write_text(r'''#import <Foundation/Foundation.h>
#include "host_benchmark_snapshot.h"
#define FOLDER "benchmark"
static NSDictionary *pinned_snapshot;
''' + function("port/ios/host/host_benchmark.m", "replay_environment_for_recording") + r'''
int main(int argc, const char **argv) {
    @autoreleasepool {
        pinned_snapshot = @{ @"files": @{ [@(argv[1]) stringByAppendingPathExtension:@"actions"]: @(argv[2]) } };
        NSDictionary *record = replay_environment_for_recording(argv[1]);
        NSData *data = [NSJSONSerialization dataWithJSONObject:record options:0 error:nil];
        puts([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
    }
    return 0;
}
''')
    output = tmp_path / "report"
    subprocess.run(["xcrun", "clang", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-fobjc-arc", "-framework", "Foundation", "-I" + str(ROOT / "port/ios/host"),
                    str(source), str(ROOT / "port/ios/host/host_benchmark_snapshot.m"), "-o", str(output)], check=True)
    import hashlib
    import json
    def reported(name="take", digest=None):
        if digest is None:
            digest = hashlib.sha256((folder / "take.actions").read_bytes()).hexdigest()
        return json.loads(subprocess.check_output([str(output), name, digest], cwd=tmp_path))
    for metadata, expected, origin in [("", False, "legacy_default"),
                                        (" enhanced_animations=0", False, "recorded"),
                                        (" enhanced_animations=1", True, "recorded")]:
        (folder / "take.actions").write_text("halo action recording 1" + metadata + "\n")
        actual = reported()
        assert actual == {"status": "available", "enhanced_animations": expected,
                          "source": origin, "scope": "while_replay_owns_simulation"}
    for metadata in [" enhanced_animations=2", " enhanced_animations=0 enhanced_animations=1", " unknown=1"]:
        (folder / "take.actions").write_text("halo action recording 1" + metadata + "\n")
        actual = reported()
        assert actual == {"status": "unavailable", "scope": "while_replay_owns_simulation"}
    (folder / "take.actions").write_text("halo action recording 1 enhanced_animations=1\n")
    assert reported(digest="0" * 64)["status"] == "unavailable"
    assert reported(digest="missing")["status"] == "unavailable"
    pinned = hashlib.sha256((folder / "take.actions").read_bytes()).hexdigest()
    (folder / "take.actions").write_text("halo action recording 1 enhanced_animations=0\n")
    assert reported(digest=pinned)["status"] == "unavailable"
    (folder / "take.actions").unlink()
    assert reported(digest=pinned)["status"] == "unavailable"
    assert reported("../take", "0" * 64)["status"] == "unavailable"
    # The actual report consumer still gives the original route/error bytes to json_string.
    write_run = function("port/ios/host/host_benchmark.m", "write_run")
    assert "json_string(note)" in write_run
    assert 'record[@"replay_environment"] = replay_environment_for_recording(scene->recording)' in write_run
    from benchmark_routes import route_points
    note = "0:1,2,3;30:4,5,6"
    assert route_points(note) == [(0.0, 1.0, 2.0, 3.0), (30.0, 4.0, 5.0, 6.0)]
