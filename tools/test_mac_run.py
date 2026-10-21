"""Tests for the Mac runner's pure helpers (tools/mac_run.py)."""

import argparse
import plistlib
import shutil
import struct
from pathlib import Path

import pytest

from tools import mac_run


CONFIG = """# comment
[display]
screen_width = 0
interpolation = true

[debug]
exit_after = 0.0
gpu_stats = false
"""


def test_merge_config_replaces_existing_key_in_its_section():
    merged = mac_run.merge_config(CONFIG, {"debug.gpu_stats": "true"})
    assert "gpu_stats = true" in merged
    assert "gpu_stats = false" not in merged
    assert "interpolation = true" in merged


def test_merge_config_adds_missing_key_to_existing_section():
    merged = mac_run.merge_config(CONFIG, {"display.vsync": "false"})
    display = merged.split("[display]")[1].split("[debug]")[0]
    assert "vsync = false" in display


def test_merge_config_adds_missing_section():
    merged = mac_run.merge_config(CONFIG, {"audio.enabled": "false"})
    assert "[audio]\nenabled = false" in merged


def test_merge_config_does_not_touch_same_key_in_other_section():
    text = "[a]\nx = 1\n[b]\nx = 2\n"
    merged = mac_run.merge_config(text, {"b.x": "3"})
    assert merged == "[a]\nx = 1\n[b]\nx = 3\n"


def test_reset_settings_turn_off_unrequested_outputs(tmp_path):
    settings = mac_run.reset_settings(tmp_path, screenshot_every=0, dump_shaders=False, replay=False)
    assert settings["debug.screenshot_every"] == "0"
    assert settings["debug.screenshot_directory"] == '""'
    assert settings["debug.gpu_dump_shaders"] == '""'
    assert settings["debug.gpu_shader_replay"] == '""'


def test_reset_settings_point_requested_outputs_into_the_runner_folder(tmp_path):
    settings = mac_run.reset_settings(tmp_path, screenshot_every=300, dump_shaders=True, replay=True)
    assert settings["debug.screenshot_every"] == "300"
    assert settings["debug.screenshot_directory"] == f'"{tmp_path}/runner/shots"'
    assert settings["debug.gpu_dump_shaders"] == f'"{tmp_path}/runner/shaders"'
    assert settings["debug.gpu_shader_replay"] == f'"{tmp_path}/runner/replay"'


def _container(root: Path, name: str, identifier: str) -> Path:
    container = root / name
    container.mkdir()
    with (container / ".com.apple.containermanagerd.metadata.plist").open("wb") as file:
        plistlib.dump({"MCMMetadataIdentifier": identifier}, file, fmt=plistlib.FMT_BINARY)
    return container


def test_find_container_matches_bundle_identifier(tmp_path):
    _container(tmp_path, "A", "org.example.other")
    wanted = _container(tmp_path, "B", "org.steverice.visr.macrunner")
    assert mac_run.find_container(tmp_path, "org.steverice.visr.macrunner") == wanted / "Data/Documents"


def test_find_container_returns_none_when_absent(tmp_path):
    _container(tmp_path, "A", "org.example.other")
    assert mac_run.find_container(tmp_path, "org.steverice.visr.macrunner") is None


def _bmp(width: int, height: int, pixels: bytes) -> bytes:
    """a 32-bit top-down BMP laid out as write_screenshot (d3d8_device.c) writes it"""
    header = bytearray(54)
    header[0:2] = b"BM"
    struct.pack_into("<I", header, 2, 54 + len(pixels))
    struct.pack_into("<I", header, 10, 54)
    struct.pack_into("<I", header, 14, 40)
    struct.pack_into("<ii", header, 18, width, -height)
    struct.pack_into("<H", header, 26, 1)
    struct.pack_into("<H", header, 28, 32)
    struct.pack_into("<I", header, 34, len(pixels))
    return bytes(header) + pixels


def test_read_bmp_returns_dimensions_and_pixels():
    pixels = bytes(range(16))
    width, height, data = mac_run.read_bmp(_bmp(2, 2, pixels))
    assert (width, height, data) == (2, 2, pixels)


def test_bmp_difference_identical_is_zero():
    image = _bmp(2, 1, b"\x01\x02\x03\xff\x04\x05\x06\xff")
    assert mac_run.bmp_difference(image, image) == (0, 0, None)


def test_bmp_difference_counts_pixels_and_largest_delta_ignoring_alpha():
    a = _bmp(2, 1, b"\x10\x10\x10\xff\x20\x20\x20\xff")
    b = _bmp(2, 1, b"\x10\x10\x10\x00\x20\x2a\x20\xff")
    assert mac_run.bmp_difference(a, b) == (1, 10, (1, 0))


def test_bmp_difference_different_sizes_counts_every_pixel():
    a = _bmp(2, 1, bytes(8))
    b = _bmp(1, 1, bytes(4))
    assert mac_run.bmp_difference(a, b) == (2, 255, None)


def test_bmp_difference_region_counts_only_its_pixels():
    # 2x2, top down: the top-left pixel differs, the bottom-right one more
    a = _bmp(2, 2, bytes(16))
    b = _bmp(2, 2, b"\x05\x00\x00\xff" + bytes(8) + b"\x09\x00\x00\xff")
    assert mac_run.bmp_difference(a, b) == (2, 9, (1, 1))
    assert mac_run.bmp_difference(a, b, region=(0.0, 0.0, 0.5, 0.5)) == (1, 5, (0, 0))
    assert mac_run.bmp_difference(a, b, region=(0.5, 0.0, 1.0, 0.5)) == (0, 0, None)
    assert mac_run.bmp_pixels(a, (0.0, 0.0, 0.5, 1.0)) == 2


def test_parse_region_accepts_fractions_and_rejects_the_rest():
    assert mac_run.parse_region("0.34,0.34,0.66,0.66") == (0.34, 0.34, 0.66, 0.66)
    for text in ("0.5,0.5,0.4,0.6", "0,0,1", "a,b,c,d", "0,0,1.5,1"):
        try:
            mac_run.parse_region(text)
        except Exception as error:
            assert "X0,Y0,X1,Y1" in str(error)
        else:
            raise AssertionError(text)


def test_compare_region_ignores_differences_outside_it(tmp_path):
    a = _result(tmp_path / "a", {}, {"frame00300.bmp": _bmp(2, 1, bytes(8))}, "")
    b = _result(tmp_path / "b", {}, {"frame00300.bmp": _bmp(2, 1, bytes(4) + b"\x30\x00\x00\xff")}, "")
    assert any("1 pixels differ" in p for p in mac_run.compare(a, b))
    assert not any("differ" in p for p in mac_run.compare(a, b, region=(0.0, 0.0, 0.5, 1.0)))


def test_stats_lines_strip_prefix_and_frame_number():
    log = "halo-linux: frame 60: 812 draws, 3 immediate, 4 GL calls\nother\nhalo-linux: frame 120: 800 draws, 3 immediate, 4 GL calls\n"
    assert mac_run.stats_lines(log) == ["812 draws, 3 immediate, 4 GL calls", "800 draws, 3 immediate, 4 GL calls"]


def _result(root: Path, shaders: dict, shots: dict, log: str) -> Path:
    (root / "runner/shaders").mkdir(parents=True)
    (root / "runner/shots").mkdir(parents=True)
    for name, text in shaders.items():
        (root / "runner/shaders" / name).write_text(text)
    for name, data in shots.items():
        (root / "runner/shots" / name).write_bytes(data)
    (root / "stderr.log").write_text(log)
    return root


def test_compare_identical_results_has_no_problems(tmp_path):
    image = _bmp(1, 1, b"\x01\x02\x03\xff")
    a = _result(tmp_path / "a", {"vs001_0.glsl": "x"}, {"frame00300.bmp": image}, "frame 60: 1 draws\n")
    b = _result(tmp_path / "b", {"vs001_0.glsl": "x"}, {"frame00300.bmp": image}, "frame 60: 1 draws\n")
    assert mac_run.compare(a, b, tolerance=0) == []


def test_compare_reports_shader_text_and_missing_files(tmp_path):
    a = _result(tmp_path / "a", {"vs001_0.glsl": "x", "ps_1.glsl": "p"}, {}, "")
    b = _result(tmp_path / "b", {"vs001_0.glsl": "y"}, {}, "")
    problems = mac_run.compare(a, b, tolerance=0)
    assert any("vs001_0.glsl differs" in p for p in problems)
    assert any("ps_1.glsl only in" in p for p in problems)


def test_compare_reports_frames_beyond_tolerance_and_missing_frames(tmp_path):
    a = _result(tmp_path / "a", {}, {"frame00300.bmp": _bmp(1, 1, b"\x00\x00\x00\xff"),
                                     "frame00600.bmp": _bmp(1, 1, bytes(4))}, "")
    b = _result(tmp_path / "b", {}, {"frame00300.bmp": _bmp(1, 1, b"\x09\x00\x00\xff")}, "")
    problems = mac_run.compare(a, b, tolerance=0)
    assert any("frame00300.bmp: 1 pixels differ" in p for p in problems)
    assert any("frame00600.bmp only in" in p for p in problems)
    assert not any("frame00300" in p for p in mac_run.compare(a, b, tolerance=1) if "differ" in p)


def test_compare_reports_stats_differences_and_missing_logs(tmp_path):
    a = _result(tmp_path / "a", {}, {}, "frame 60: 1 draws\n")
    b = _result(tmp_path / "b", {}, {}, "frame 60: 2 draws\n")
    assert any("gpu_stats" in p for p in mac_run.compare(a, b, tolerance=0))
    (b / "stderr.log").unlink()
    assert any("stderr.log missing" in p for p in mac_run.compare(a, b, tolerance=0))


def test_prepare_creates_the_requested_output_folders(tmp_path):
    """the game writes into these folders but does not create them"""
    args = mac_run.argparse.Namespace(xiso=None, replay=None, screenshot_every=300, dump_shaders=True,
                                      exit_after=10.0, set=[], init=[])
    mac_run.prepare(args, tmp_path)
    assert (tmp_path / "runner/shots").is_dir()
    assert (tmp_path / "runner/shaders").is_dir()
    assert (tmp_path / "stderr.log").is_file()


def test_compare_reports_truncated_and_empty_frames(tmp_path):
    """a run killed mid-write leaves a partial BMP behind"""
    image = _bmp(2, 2, bytes(16))
    a = _result(tmp_path / "a", {}, {"frame00300.bmp": image, "frame00600.bmp": image}, "")
    b = _result(tmp_path / "b", {}, {"frame00300.bmp": image[:60], "frame00600.bmp": b""}, "")
    problems = mac_run.compare(a, b, tolerance=0)
    assert any("frame00300.bmp: unreadable" in p for p in problems)
    assert any("frame00600.bmp: unreadable" in p for p in problems)


def test_reset_settings_restore_defaults_an_earlier_set_may_have_changed(tmp_path):
    """--set writes into the container's config.toml, which the next run reuses"""
    settings = mac_run.reset_settings(tmp_path, screenshot_every=0, dump_shaders=False, replay=False)
    assert settings["debug.gpu_stats"] == "false"
    assert settings["debug.gpu_debug_flat"] == "false"
    assert settings["debug.gpu_skip_vertex_shaders"] == '""'
    assert settings["debug.gpu_trace_frame"] == "-1"
    assert settings["display.interpolation"] == "true"
    assert settings["display.render_height"] == "0"


def test_launch_script_waits_for_xcode_to_open_the_project():
    """open -a returns before Xcode has the project open when it wasn't already"""
    script = mac_run.LAUNCH.format(xcode="/Applications/Xcode.app", target="HaloRunner", project="/p/HaloRunner.xcodeproj")
    wait = script.index('exists (first workspace document whose path is "/p/HaloRunner.xcodeproj")')
    assert wait < script.index("set doc to")


def test_reset_settings_turn_off_the_fixed_timestep(tmp_path):
    """a run that doesn't ask for the virtual clock must get the real one"""
    settings = mac_run.reset_settings(tmp_path, screenshot_every=0, dump_shaders=False, replay=False)
    assert settings["debug.fixed_timestep"] == "false"


def test_reset_settings_turn_off_the_texture_override(tmp_path):
    """a run that doesn't --set an override folder must draw the game's own textures"""
    settings = mac_run.reset_settings(tmp_path, screenshot_every=0, dump_shaders=False, replay=False)
    assert settings["debug.texture_override_directory"] == '""'


def test_reset_settings_turn_the_upscaled_textures_back_on(tmp_path):
    """a run that turned upscaled textures off must not leak into the next"""
    settings = mac_run.reset_settings(tmp_path, screenshot_every=0, dump_shaders=False, replay=False)
    assert settings["display.upscaled_textures"] == "true"


def test_launch_script_fails_when_the_ipad_destination_never_appears():
    """after the retries, a last unguarded attempt raises instead of running on another destination"""
    script = mac_run.LAUNCH.format(xcode="/Applications/Xcode.app", target="HaloRunner", project="/p/HaloRunner.xcodeproj")
    retries_end = script.index("end repeat", script.index("end try"))
    final = 'set active run destination of doc to (first run destination of doc whose name is "My Mac (Designed for iPad)")'
    assert script.index(final, retries_end) < script.index("run doc")


def test_prepare_removes_the_previous_runtime_log(tmp_path):
    """a fresh ios-runtime.log is how a run that ends between polls is seen to have started"""
    (tmp_path / "ios-runtime.log").write_text("game exit 0\n")
    args = mac_run.argparse.Namespace(xiso=None, replay=None, screenshot_every=0, dump_shaders=False,
                                      exit_after=10.0, set=[], init=[])
    mac_run.prepare(args, tmp_path)
    assert not (tmp_path / "ios-runtime.log").exists()


def test_started_sees_a_run_that_already_finished(tmp_path):
    assert not mac_run.started(tmp_path, lambda: False)
    assert mac_run.started(tmp_path, lambda: True)
    (tmp_path / "ios-runtime.log").write_text("game exit 0\n")
    assert mac_run.started(tmp_path, lambda: False)
    assert mac_run.started(None, lambda: True)
    assert not mac_run.started(None, lambda: False)


def test_launch_script_drives_this_checkout_s_project_only(tmp_path):
    """Xcode keeps one project named HaloRunner open: another worktree's would be driven instead"""
    project = tmp_path / "build/mac-runner/HaloRunner.xcodeproj"
    close = mac_run.CLOSE_OTHERS.format(xcode="/Applications/Xcode.app", target="HaloRunner", project=project)
    assert f'whose path contains "/HaloRunner.xcodeproj" and path does not start with "{project}"' in close
    launch = mac_run.LAUNCH.format(xcode="/Applications/Xcode.app", target="HaloRunner", project=project)
    assert "whose path contains" not in launch
    assert launch.count(f'whose path is "{project}"') == 2


def test_close_script_closes_a_runner_project_xcode_lists_as_its_workspace():
    """Xcode listed a deleted worktree's runner project as "project.xcworkspace", so closing by
    name missed it and this project's run stayed "not yet started" """
    project = "/w/upstream-merge/build/mac-runner/HaloRunner.xcodeproj"
    close = mac_run.CLOSE_OTHERS.format(xcode="/Applications/Xcode.app", target="HaloRunner", project=project)
    assert "whose name is" not in close
    assert "try\n\t\t\tclose other saving no" in close


def test_launch_script_retries_when_xcode_cannot_run():
    """Xcode sometimes answers a run with "Cannot run 'HaloRunner'" right after opening the project"""
    script = mac_run.LAUNCH.format(xcode="/Applications/Xcode.app", target="HaloRunner", project="/p/HaloRunner.xcodeproj")
    assert "set result_of_run to run doc" in script
    assert 'else if run_status is not "error occurred" then' in script
    assert script.index("repeat 3 times") < script.index("set result_of_run to run doc")


def test_launch_script_retries_a_cancelled_run():
    """a project reload right after the run was asked for cancels it before anything launches,
    which counted as started and left the runner waiting 300 s (M4 Mac mini, 2026-10-05)"""
    script = mac_run.LAUNCH.format(xcode="/Applications/Xcode.app", target="HaloRunner", project="/p/HaloRunner.xcodeproj")
    cancelled = script.index('if run_status is "cancelled" then')
    assert cancelled < script.index("set started_run to true")
    assert script.index('set last_error to "Xcode cancelled the run"') > cancelled


def test_launch_script_fails_with_the_reason_after_three_attempts():
    """giving up quietly left the runner waiting 300 s with the cause lost"""
    script = mac_run.LAUNCH.format(xcode="/Applications/Xcode.app", target="HaloRunner", project="/p/HaloRunner.xcodeproj")
    assert "on error message_of_error" in script
    assert 'error "run failed after 3 attempts: " & last_error' in script
    assert script.index("end repeat", script.index("repeat 3 times")) < script.index('error "run failed after 3 attempts')


def test_compare_can_ignore_only_the_gl_call_total(tmp_path):
    a = _result(tmp_path / "a", {}, {}, "frame 60: 5 draws, 2 immediate; 10 KB streamed, 400 GL calls\n")
    b = _result(tmp_path / "b", {}, {}, "frame 60: 5 draws, 2 immediate; 10 KB streamed, 391 GL calls\n")
    c = _result(tmp_path / "c", {}, {}, "frame 60: 6 draws, 2 immediate; 10 KB streamed, 391 GL calls\n")
    assert any("gpu_stats" in p for p in mac_run.compare(a, b, tolerance=0))
    assert mac_run.compare(a, b, tolerance=0, ignore_gl_calls=True) == []
    assert any("gpu_stats" in p for p in mac_run.compare(a, c, tolerance=0, ignore_gl_calls=True))


def test_bmp_difference_ignores_pixels_within_the_channel_tolerance():
    a = _bmp(3, 1, b"\x10\x10\x10\xff\x20\x20\x20\xff\x30\x30\x30\xff")
    b = _bmp(3, 1, b"\x12\x10\x10\xff\x20\x23\x20\xff\x30\x30\x30\xff")
    assert mac_run.bmp_difference(a, b, channel_tolerance=2) == (1, 3, (1, 0))


def test_compare_allows_a_fraction_of_pixels_beyond_the_channel_tolerance(tmp_path):
    """the GL-against-Metal threshold: within 2 levels, except a few pixels"""
    base = bytes([0x40, 0x40, 0x40, 0xff]) * 1000
    near = bytearray(base)
    near[0] = 0x42                      # within 2 levels: not a differing pixel
    far = bytearray(near)
    far[4] = 0x60                       # one pixel off by 32
    a = _result(tmp_path / "a", {}, {"frame00300.bmp": _bmp(1000, 1, base)}, "")
    b = _result(tmp_path / "b", {}, {"frame00300.bmp": _bmp(1000, 1, bytes(near))}, "")
    c = _result(tmp_path / "c", {}, {"frame00300.bmp": _bmp(1000, 1, bytes(far))}, "")
    assert mac_run.compare(a, b, channel_tolerance=2) == []
    problems = mac_run.compare(a, c, channel_tolerance=2)
    assert any("frame00300.bmp: 1 pixels differ, by up to 32 at 1,0" in p for p in problems)
    assert mac_run.compare(a, c, channel_tolerance=2, fraction=0.001) == []


def test_compare_across_backends_checks_shader_inputs_and_names_not_sources(tmp_path):
    log = "halo-linux: GPU capabilities: copy image 0, anisotropy 1\nframe 60: 5 draws, 7 GL calls\n"
    gl = _result(tmp_path / "gl", {"vs001_0.glsl": "#version 300 es", "vs001_0.vsh": "v", "ps_1.glsl": "g",
                                   "ps_1.key": "k"}, {}, log)
    metal = _result(tmp_path / "metal", {"vs001_0.metal": "#include <metal_stdlib>", "vs001_0.vsh": "v",
                                         "ps_1.metal": "m", "ps_1.key": "k"}, {},
                    log.replace("7 GL calls", "0 GL calls"))
    assert mac_run.compare(gl, metal, across_backends=True) == []
    (metal / "runner/shaders/ps_1.key").write_text("other")
    (metal / "runner/shaders/ps_1.metal").rename(metal / "runner/shaders/ps_2.metal")
    problems = mac_run.compare(gl, metal, across_backends=True)
    assert any("ps_1.key differs" in p for p in problems)
    assert any("ps_1 only in" in p for p in problems)
    assert any("ps_2 only in" in p for p in problems)


def test_compare_ignores_immediate_draw_counts_only_when_asked(tmp_path):
    log = "frame 60: 341 draws, 51 immediate, 3 clears, 8 target changes, 7 GL calls\n"
    a = _result(tmp_path / "a", {"ps_1.metal": "m"}, {}, log)
    b = _result(tmp_path / "b", {"ps_1.metal": "m"}, {}, log.replace("51 immediate", "47 immediate"))
    assert any("gpu_stats differ" in p for p in mac_run.compare(a, b))
    assert mac_run.compare(a, b, ignore_immediate_draws=True) == []
    (b / "runner/shaders/ps_1.metal").write_text("other")
    assert any("ps_1.metal differs" in p for p in mac_run.compare(a, b, ignore_immediate_draws=True))
    c = _result(tmp_path / "c", {"ps_1.metal": "m"}, {}, log.replace("341 draws", "340 draws"))
    assert any("gpu_stats differ" in p for p in mac_run.compare(a, c, ignore_immediate_draws=True))


def test_compare_across_backends_requires_matching_capabilities(tmp_path):
    gl = _result(tmp_path / "gl", {}, {}, "GPU capabilities: copy image 0, anisotropy 1\n")
    metal = _result(tmp_path / "metal", {}, {}, "GPU capabilities: copy image 0, anisotropy 0\n")
    silent = _result(tmp_path / "silent", {}, {}, "")
    assert any("capabilities differ" in p for p in mac_run.compare(gl, metal, across_backends=True))
    assert any("capabilities differ" in p for p in mac_run.compare(silent, silent, across_backends=True))


def test_compare_across_backends_allows_the_capabilities_only_metal_has(tmp_path):
    line = "GPU capabilities: vertex BGRA {0}, base vertex {0}, triangle fans 1, max texture size {1}\n" \
        "frame 60: 5 draws, 2 immediate; skipped 0 link; {2} KB mirrored, {3} KB streamed, {4} GL calls\n"
    gl = _result(tmp_path / "gl", {}, {}, line.format(0, 16384, 10, 90, 7))
    metal = _result(tmp_path / "metal", {}, {}, line.format(1, 16384, 80, 20, 0))
    other = _result(tmp_path / "other", {}, {}, line.format(1, 8192, 80, 20, 0))
    assert mac_run.compare(gl, metal, across_backends=True) == []
    assert any("capabilities differ" in p for p in mac_run.compare(gl, other, across_backends=True))
    # two runs of one backend still compare the whole lines
    assert any("gpu_stats differ" in p for p in mac_run.compare(gl, metal, ignore_gl_calls=True))


def test_compare_same_backend_compares_metal_sources_too(tmp_path):
    a = _result(tmp_path / "a", {"ps_1.metal": "x"}, {}, "")
    b = _result(tmp_path / "b", {"ps_1.metal": "y"}, {}, "")
    assert any("ps_1.metal differs" in p for p in mac_run.compare(a, b))


def test_reset_settings_return_to_the_gl_renderer(tmp_path):
    settings = mac_run.reset_settings(tmp_path, screenshot_every=0, dump_shaders=False, replay=False)
    assert settings["display.renderer"] == '"gl"'


def test_project_turns_on_metal_validation_only_when_asked():
    plain = mac_run.PROJECT.format(target="T", team="X", bundle_id="b", app="/a", executable="VISR",
                                   environment=mac_run.scheme_environment({}))
    validated = mac_run.PROJECT.format(target="T", team="X", bundle_id="b", app="/a", executable="VISR",
                                       environment=mac_run.scheme_environment(mac_run.METAL_VALIDATION))
    assert "environmentVariables" not in plain
    assert '        MTL_DEBUG_LAYER: "1"\n' in validated
    assert validated.index("debugEnabled: false") < validated.index("environmentVariables")


def test_project_copies_the_apps_own_executable():
    project = mac_run.PROJECT.format(target="T", team="X", bundle_id="b", app="/a", executable="HaloCE",
                                     environment=mac_run.scheme_environment({}))
    assert 'cp "/a/HaloCE" "$TARGET_BUILD_DIR/$EXECUTABLE_PATH"' in project


def test_app_executable_reads_the_plist_and_defaults_to_visr(tmp_path):
    old = tmp_path / "HaloCE.app"
    old.mkdir()
    with (old / "Info.plist").open("wb") as file:
        plistlib.dump({"CFBundleExecutable": "HaloCE"}, file)
    assert mac_run.app_executable(old) == "HaloCE"
    assert mac_run.app_executable(tmp_path / "VISR.app") == "VISR"


def test_shader_validation_is_its_own_flag():
    api = mac_run.validation_environment(metal_validation=True, metal_shader_validation=False)
    shader = mac_run.validation_environment(metal_validation=False, metal_shader_validation=True)
    assert api["MTL_DEBUG_LAYER"] == "1" and "MTL_SHADER_VALIDATION" not in api
    assert shader["MTL_SHADER_VALIDATION"] == "1" and shader["MTL_DEBUG_LAYER"] == "1"
    assert mac_run.validation_environment(metal_validation=False, metal_shader_validation=False) == {}


def test_simulator_pattern_matches_only_that_simulators_game():
    import re
    udid = "3BA81FB2-1A04-49EF-BF13-5B2598B8FBF9"
    path = (f"/Users/x/Library/Developer/CoreSimulator/Devices/{udid}/data/Containers/Bundle/Application/"
            "47C658A7-DADC-45F2-969B-7E6A4FB5CB5C/VISR.app/VISR")
    assert re.search(mac_run.simulator_pattern(udid), path)
    assert not re.search(mac_run.simulator_pattern("00000000-0000-0000-0000-000000000000"), path)
    assert not re.search(mac_run.simulator_pattern(udid), path.replace("VISR.app/VISR", "Other.app/Other"))


def test_simulator_run_refuses_a_missing_app(tmp_path):
    import argparse
    args = argparse.Namespace(simulator="3BA81FB2-1A04-49EF-BF13-5B2598B8FBF9", app=tmp_path / "VISR.app")
    try:
        mac_run.run_simulator(args)
    except SystemExit as stop:
        assert "no simulator app" in str(stop)
    else:
        raise AssertionError("run_simulator accepted a missing app")


def make_native_app(root, bundle_id):
    """a stand-in for the Catalyst app: its executable and Info.plist"""
    app = root / "build/VISR.app"
    (app / "Contents/MacOS").mkdir(parents=True)
    (app / "Contents/MacOS/VISR").write_text("")
    with (app / "Contents/Info.plist").open("wb") as file:
        plistlib.dump({"CFBundleIdentifier": bundle_id}, file)
    return app


def native_args(root, app, **overrides):
    values = dict(runner="native", app=app, bundle_id=None, maps=None, out=root / "out", xiso=None, team=None,
                  simulator=None, exit_after=40.0, time_limit=0.0, set=[], init=[], screenshot_every=0,
                  dump_shaders=False, replay=None, metal_validation=False, metal_shader_validation=False)
    values.update(overrides)
    return argparse.Namespace(**values)


def fake_launches(launches, exit_line="halo-ios: game exit 0"):
    """launch_native without a game: records what each launch would have read, writes the game's log"""
    def launch_native(app, data, out, environment, limit):
        init = data / "init.txt"
        launches.append({"init": init.read_text() if init.exists() else "", "out": out,
                         "config": (data / "config.toml").read_text(), "environment": environment})
        (data / "ios-runtime.log").write_text(f"Halo iOS native guest starting\n{exit_line}\n")
        return True
    return launch_native


@pytest.fixture
def native(tmp_path, monkeypatch):
    """a native app, a seeded data folder, no running copy, and Xcode out of reach"""
    def no_xcode(*args, **kwargs):
        raise AssertionError("the native runner drove Xcode")
    monkeypatch.setattr(mac_run, "launch", no_xcode)
    monkeypatch.setattr(mac_run, "build_wrapper", no_xcode)
    monkeypatch.setattr(mac_run, "native_pids", lambda app: [])
    monkeypatch.setenv("HALO_NATIVE_DATA", str(tmp_path / "data"))
    (tmp_path / "data/maps").mkdir(parents=True)
    (tmp_path / "data/seeded").write_text("seeded by the test fixture\n")
    return make_native_app(tmp_path, "org.example.mac")


def test_native_data_folder_defaults_to_application_support(tmp_path):
    assert mac_run.native_data_folder("org.example.mac", {}, tmp_path) == \
        tmp_path / "Library/Application Support/org.example.mac/runner-data"


def test_native_data_folder_takes_a_remote_host_s_folder_from_the_environment(tmp_path):
    assert mac_run.native_data_folder("org.example.mac", {"HALO_NATIVE_DATA": str(tmp_path / "d")}, tmp_path) == \
        tmp_path / "d"


def test_native_command_pins_the_display_and_passes_the_data_folder(tmp_path):
    data = tmp_path / "Application Support/runner-data"
    command = mac_run.native_command(Path("/b/VISR.app"), data, tmp_path / "out", {"MTL_DEBUG_LAYER": "1"})
    assert command[:3] == ["open", "--new", "--wait-apps"]
    variables = [command[i + 1] for i, part in enumerate(command) if part == "--env"]
    assert f"HALO_DATA_ROOT={data}" in variables
    assert "HALO_HOST_DISPLAY=1366x1024@2" in variables
    assert "HALO_RUNNER=1" in variables
    assert "MTL_DEBUG_LAYER=1" in variables
    # the logs go to the data folder (internal disk), not --out, which may be on a removable volume
    assert command[command.index("--stdout") + 1] == str(data / "open-stdout.log")
    assert command[command.index("--stderr") + 1] == str(data / "open-stderr.log")
    assert command[-1] == "/b/VISR.app"


def fake_open(monkeypatch, stdout, stderr, status=0):
    """subprocess.Popen standing in for open: writes the log files its command names, as launchd would"""
    class Opener:
        def __init__(self, command):
            Path(command[command.index("--stdout") + 1]).write_text(stdout)
            Path(command[command.index("--stderr") + 1]).write_text(stderr)

        def wait(self, timeout=None):
            return status

    monkeypatch.setattr(mac_run.subprocess, "Popen", Opener)


def test_launch_native_copies_the_open_logs_into_out_and_replaces_the_old_ones(tmp_path, monkeypatch):
    data, out = tmp_path / "data", tmp_path / "out"
    data.mkdir()
    (data / "open-stdout.log").write_text("stale stdout\n")
    (data / "open-stderr.log").write_text("stale stderr\n")
    fake_open(monkeypatch, "fresh stdout\n", "fresh stderr\n")
    assert mac_run.launch_native(Path("/b/VISR.app"), data, out, {}, 10)
    assert (out / "open-stdout.log").read_text() == "fresh stdout\n"
    assert (out / "open-stderr.log").read_text() == "fresh stderr\n"
    # the data folder's copies hold this launch's content, not the stale files or an append
    assert (data / "open-stdout.log").read_text() == "fresh stdout\n"
    assert (data / "open-stderr.log").read_text() == "fresh stderr\n"


def test_launch_native_copies_the_open_logs_when_open_fails(tmp_path, monkeypatch):
    data, out = tmp_path / "data", tmp_path / "out"
    data.mkdir()
    fake_open(monkeypatch, "", "open: -10810\n", status=1)
    with pytest.raises(SystemExit):
        mac_run.launch_native(Path("/b/VISR.app"), data, out, {}, 10)
    assert (out / "open-stderr.log").read_text() == "open: -10810\n"
    assert (data / "open-stderr.log").read_text() == "open: -10810\n"


def test_native_pattern_escapes_the_app_path():
    assert mac_run.native_pattern(Path("/a.b/Halo (1).app")) == r"/a\.b/Halo \(1\)\.app/Contents/MacOS/VISR"


def test_game_exit_reads_the_game_s_last_exit_line():
    assert mac_run.game_exit("halo-ios: starting\nhalo-ios: game exit 0\n") == 0
    assert mac_run.game_exit("game exit 3") == 3
    assert mac_run.game_exit("halo-ios: FATAL: out of guest memory\n") is None


def test_prepare_rewrite_drops_settings_an_earlier_run_left(tmp_path):
    (tmp_path / "config.toml").write_text("[debug]\nnetwork_test = \"host:bloodgulch\"\n")
    args = argparse.Namespace(xiso=None, screenshot_every=0, dump_shaders=False, replay=None, exit_after=5,
                              set=["display.renderer=\"metal\""], init=[])
    mac_run.prepare(args, tmp_path, rewrite=True)
    text = (tmp_path / "config.toml").read_text()
    # DEFAULTS resets debug.network_test itself, to "": the earlier run's value is gone either way
    assert "bloodgulch" not in text
    assert "exit_after = 5.0" in text and 'renderer = "metal"' in text


def test_prepare_still_merges_for_the_ipad_runner(tmp_path):
    (tmp_path / "config.toml").write_text("[debug]\nnetwork_test = \"host:bloodgulch\"\n")
    args = argparse.Namespace(xiso=None, screenshot_every=0, dump_shaders=False, replay=None, exit_after=5,
                              set=[], init=[])
    mac_run.prepare(args, tmp_path)
    assert "network_test" in (tmp_path / "config.toml").read_text()


def test_native_run_never_drives_xcode(tmp_path, monkeypatch, native):
    launches = []
    monkeypatch.setattr(mac_run, "launch_native", fake_launches(launches))
    mac_run.run_native(native_args(tmp_path, native, metal_validation=True))
    assert len(launches) == 1
    assert launches[0]["environment"]["MTL_DEBUG_LAYER"] == "1"
    assert (tmp_path / "out/ios-runtime.log").is_file()


def test_native_run_refuses_while_the_app_is_still_running(tmp_path, monkeypatch, native):
    monkeypatch.setattr(mac_run, "native_pids", lambda app: [4242])
    monkeypatch.setattr(mac_run, "launch_native", fake_launches([]))
    with pytest.raises(SystemExit, match=r"already running \(pid 4242\)"):
        mac_run.run_native(native_args(tmp_path, native))


def test_native_run_fails_when_the_game_does_not_exit_cleanly(tmp_path, monkeypatch, native):
    monkeypatch.setattr(mac_run, "launch_native", fake_launches([], exit_line="halo-ios: FATAL: no maps"))
    with pytest.raises(SystemExit, match="did not exit cleanly"):
        mac_run.run_native(native_args(tmp_path, native))
    assert (tmp_path / "out/ios-runtime.log").is_file()    # the logs are collected anyway


def test_native_run_refuses_an_app_built_for_another_bundle_id(tmp_path, monkeypatch, native):
    monkeypatch.setattr(mac_run, "launch_native", fake_launches([]))
    with pytest.raises(SystemExit, match="built for org.example.mac"):
        mac_run.run_native(native_args(tmp_path, native, bundle_id="org.example.other"))


def test_native_run_rewrites_config_each_run(tmp_path, monkeypatch, native):
    launches = []
    monkeypatch.setattr(mac_run, "launch_native", fake_launches(launches))
    mac_run.run_native(native_args(tmp_path, native, set=['debug.input_replay="a10.rec"']))
    mac_run.run_native(native_args(tmp_path, native))
    assert "a10.rec" in launches[0]["config"]
    assert "a10.rec" not in launches[1]["config"]


def test_native_replay_mouth_opt_in_does_not_leak_to_next_run(tmp_path, monkeypatch, native):
    import tomllib
    launches = []
    monkeypatch.setattr(mac_run, "launch_native", fake_launches(launches))
    mac_run.run_native(native_args(tmp_path, native, set=['debug.input_replay="a10.input"']))
    mac_run.run_native(native_args(tmp_path, native))
    assert tomllib.loads(launches[0]["config"])["debug"]["benchmark_lip_sync"] is True
    assert tomllib.loads(launches[1]["config"])["debug"]["benchmark_lip_sync"] is False


def test_native_replay_can_explicitly_keep_original_mouths(tmp_path, monkeypatch, native):
    import tomllib
    launches = []
    monkeypatch.setattr(mac_run, "launch_native", fake_launches(launches))
    mac_run.run_native(native_args(tmp_path, native, set=[
        'debug.input_replay="a10.input"', 'debug.benchmark_lip_sync=false']))
    assert tomllib.loads(launches[0]["config"])["debug"]["benchmark_lip_sync"] is False


def seeding(monkeypatch, tmp_path):
    """an empty data folder, extracted maps to seed it from, and cp -c -R done by copytree"""
    shutil.rmtree(tmp_path / "data/maps")
    (tmp_path / "data/seeded").unlink()
    maps = tmp_path / "extracted/maps"
    maps.mkdir(parents=True)
    (maps / "a10.map").write_text("map")
    monkeypatch.setattr(mac_run, "run_command",
                        lambda *command, **options: shutil.copytree(command[3], command[4]))
    return maps


def test_native_run_seeds_a_new_data_folder_once_with_throwaway_a10_and_menu_runs(tmp_path, monkeypatch, native):
    maps = seeding(monkeypatch, tmp_path)
    launches = []
    monkeypatch.setattr(mac_run, "launch_native", fake_launches(launches))
    mac_run.run_native(native_args(tmp_path, native, maps=maps))
    assert (tmp_path / "data/maps/a10.map").is_file()
    assert [launch["init"] for launch in launches] == ["map_name a10\n", "", ""]
    assert launches[0]["out"] == tmp_path / "out-seed"
    assert launches[1]["out"] == tmp_path / "out-seed-menu"
    assert launches[2]["out"] == tmp_path / "out"
    assert (tmp_path / "data/seeded").read_text().strip()
    assert "exit_after = 20.0" in launches[1]["config"]
    assert "fixed_timestep = true" in launches[1]["config"]
    assert "screenshot_every = 0" in launches[1]["config"]
    mac_run.run_native(native_args(tmp_path, native, maps=maps))
    assert len(launches) == 4    # seeded once


def test_an_interrupted_seed_is_redone(tmp_path, monkeypatch, native):
    maps = seeding(monkeypatch, tmp_path)
    (tmp_path / "data/maps.partial").mkdir()
    (tmp_path / "data/maps.partial/half.map").write_text("half")
    monkeypatch.setattr(mac_run, "launch_native", fake_launches([]))
    mac_run.run_native(native_args(tmp_path, native, maps=maps))
    assert (tmp_path / "data/maps/a10.map").is_file()
    assert not (tmp_path / "data/maps/half.map").exists()
    assert not (tmp_path / "data/maps.partial").exists()


def test_a_failed_throwaway_run_leaves_the_folder_unseeded(tmp_path, monkeypatch, native):
    maps = seeding(monkeypatch, tmp_path)
    monkeypatch.setattr(mac_run, "launch_native", fake_launches([], exit_line="halo-ios: FATAL: no maps"))
    with pytest.raises(SystemExit, match="throwaway a10 run"):
        mac_run.run_native(native_args(tmp_path, native, maps=maps))
    assert not (tmp_path / "data/maps").exists()
    assert (tmp_path / "out-seed/ios-runtime.log").is_file()


def test_a_failed_menu_seed_run_leaves_the_folder_unseeded(tmp_path, monkeypatch, native):
    maps = seeding(monkeypatch, tmp_path)
    launches = []
    good = fake_launches(launches)
    bad = fake_launches(launches, exit_line="halo-ios: FATAL: no maps")

    def launch(app, data, out, environment, limit):
        return (bad if out.name == "out-seed-menu" else good)(app, data, out, environment, limit)
    monkeypatch.setattr(mac_run, "launch_native", launch)
    with pytest.raises(SystemExit, match="throwaway menu run"):
        mac_run.run_native(native_args(tmp_path, native, maps=maps))
    assert len(launches) == 2
    assert not (tmp_path / "data/maps").exists()
    assert (tmp_path / "data/maps.partial/a10.map").is_file()
    assert (tmp_path / "out-seed-menu/ios-runtime.log").is_file()


def test_native_run_without_maps_says_how_to_seed(tmp_path, monkeypatch, native):
    shutil.rmtree(tmp_path / "data/maps")
    (tmp_path / "data/seeded").unlink()
    monkeypatch.setattr(mac_run, "launch_native", fake_launches([]))
    with pytest.raises(SystemExit, match="--maps"):
        mac_run.run_native(native_args(tmp_path, native))


def test_a_throwaway_run_that_open_could_not_start_leaves_the_folder_unseeded(tmp_path, monkeypatch, native):
    maps = seeding(monkeypatch, tmp_path)

    def open_fails(app, data, out, environment, limit):
        raise SystemExit("open could not start the app (exit 1)")
    monkeypatch.setattr(mac_run, "launch_native", open_fails)
    with pytest.raises(SystemExit, match="open could not start"):
        mac_run.run_native(native_args(tmp_path, native, maps=maps))
    assert not (tmp_path / "data/maps").exists()
    assert (tmp_path / "data/maps.partial/a10.map").is_file()


def test_a_fresh_folder_gets_the_marker_only_after_both_throwaway_runs(tmp_path, monkeypatch, native):
    maps = seeding(monkeypatch, tmp_path)
    seen = []
    good = fake_launches([])

    def launch(app, data, out, environment, limit):
        seen.append((out.name, (data / "seeded").exists()))
        return good(app, data, out, environment, limit)
    monkeypatch.setattr(mac_run, "launch_native", launch)
    mac_run.run_native(native_args(tmp_path, native, maps=maps))
    assert seen == [("out-seed", False), ("out-seed-menu", False), ("out", True)]


def test_a_folder_with_maps_and_no_marker_plays_the_throwaway_runs_without_cloning(tmp_path, monkeypatch, native):
    (tmp_path / "data/seeded").unlink()
    (tmp_path / "data/maps/mine.map").write_text("made elsewhere")

    def no_clone(*command, **options):
        raise AssertionError("cloned into a folder that already has maps")
    monkeypatch.setattr(mac_run, "run_command", no_clone)
    launches = []
    monkeypatch.setattr(mac_run, "launch_native", fake_launches(launches))
    mac_run.run_native(native_args(tmp_path, native))    # no --maps
    assert [launch["init"] for launch in launches] == ["map_name a10\n", "", ""]
    assert launches[0]["out"] == tmp_path / "out-seed"
    assert launches[1]["out"] == tmp_path / "out-seed-menu"
    assert (tmp_path / "data/seeded").is_file()
    assert (tmp_path / "data/maps/mine.map").read_text() == "made elsewhere"


def test_a_folder_with_the_marker_is_not_seeded_again(tmp_path, monkeypatch, native):
    launches = []
    monkeypatch.setattr(mac_run, "launch_native", fake_launches(launches))
    mac_run.run_native(native_args(tmp_path, native))
    assert [launch["out"] for launch in launches] == [tmp_path / "out"]


def test_a_failed_throwaway_run_leaves_existing_maps_alone_and_writes_no_marker(tmp_path, monkeypatch, native):
    (tmp_path / "data/seeded").unlink()
    (tmp_path / "data/maps/mine.map").write_text("made elsewhere")
    monkeypatch.setattr(mac_run, "launch_native", fake_launches([], exit_line="halo-ios: FATAL: no maps"))
    with pytest.raises(SystemExit, match="throwaway a10 run"):
        mac_run.run_native(native_args(tmp_path, native))
    assert (tmp_path / "data/maps/mine.map").read_text() == "made elsewhere"
    assert not (tmp_path / "data/maps.partial").exists()
    assert not (tmp_path / "data/seeded").exists()


SHA = "ab" * 32
INPUT_LOG = f"""halo-ios: Halo iOS native guest starting
halo-ios: guest image sha256 {SHA}
halo-ios: display pinned to 1366x1024@2: 2732x2048 pixels, width 640
halo-ios: guest display: width 640, pixels 2732x2048
screen: 640x480 drawn at 2732x2048 (maximum texture size 16384)
OpenGL ES 3.0 Metal - 102 on Apple M6
OpenGL ES 3.0: copy image 0, border clamp 0, anisotropy 1, S3TC 0, sample counting 1
OpenGL function glDrawElementsBaseVertex is unavailable
GPU capabilities: base vertex 0
halo-ios: audio device: 48000 Hz, 2 channels, format 0x8120, 512 sample frames
halo-ios: audio output active: 4096 PCM bytes, peak 0.2500
frame 30: 120 draws, 3 immediate, 4000 GL calls
"""
INPUT_CONFIG = """[debug]
exit_after = 40.0
screenshot_directory = "{root}/runner/shots"
gpu_dump_shaders = "{root}/runner/shaders"
"""


def input_run(folder, log=INPUT_LOG, root="/data"):
    folder.mkdir(parents=True)
    (folder / "stderr.log").write_text(log)
    (folder / "config.toml").write_text(INPUT_CONFIG.format(root=root))
    return folder


def test_input_lines_keep_the_lines_that_name_a_run_s_inputs():
    lines = mac_run.input_lines(INPUT_LOG)
    assert f"guest image sha256 {SHA}" in lines
    assert "audio device: 48000 Hz, 2 channels, format 0x8120, 512 sample frames" in lines
    assert "OpenGL function glDrawElementsBaseVertex is unavailable" in lines
    assert not any("display pinned" in line or "frame 30" in line for line in lines)


def test_runs_that_differ_only_in_their_data_folder_and_the_pin_match(tmp_path):
    a = input_run(tmp_path / "ipad", INPUT_LOG.replace("halo-ios: display pinned to 1366x1024@2: 2732x2048 pixels, width 640\n", ""),
                  root="/Users/x/Library/Containers/ABC/Data/Documents")
    b = input_run(tmp_path / "native", root="/Users/x/Library/Application Support/org.example.mac/runner-data")
    assert mac_run.compare_inputs(a, b) == []


def test_another_guest_image_is_a_difference(tmp_path):
    a = input_run(tmp_path / "a")
    b = input_run(tmp_path / "b", INPUT_LOG.replace(SHA, "cd" * 32))
    problems = mac_run.compare_inputs(a, b)
    assert len(problems) == 1 and "guest image sha256" in problems[0]


def test_a_gl_entry_point_found_by_only_one_runner_is_a_difference(tmp_path):
    a = input_run(tmp_path / "a")
    b = input_run(tmp_path / "b", INPUT_LOG.replace("OpenGL function glDrawElementsBaseVertex is unavailable\n", ""))
    assert mac_run.compare_inputs(a, b)


def test_a_setting_only_one_run_had_is_a_difference(tmp_path):
    a = input_run(tmp_path / "a")
    b = input_run(tmp_path / "b")
    (b / "config.toml").write_text((b / "config.toml").read_text() + 'network_test = "host:bloodgulch"\n')
    problems = mac_run.compare_inputs(a, b)
    assert problems == ['config.toml debug.network_test: None against "host:bloodgulch"']


def test_a_log_with_none_of_the_lines_is_a_problem_not_a_match(tmp_path):
    a = input_run(tmp_path / "a", "nothing\n")
    b = input_run(tmp_path / "b", "nothing\n")
    assert any("none of the run's inputs" in problem for problem in mac_run.compare_inputs(a, b))


def test_a_kind_of_input_both_logs_lack_is_a_problem(tmp_path):
    log = INPUT_LOG.replace("halo-ios: audio device: 48000 Hz, 2 channels, format 0x8120, 512 sample frames\n", "")
    a = input_run(tmp_path / "a", log)
    b = input_run(tmp_path / "b", log)
    problems = mac_run.compare_inputs(a, b)
    assert problems == [f"{a}/stderr.log does not name the audio device", f"{b}/stderr.log does not name the audio device"]


def test_compare_inputs_reports_missing_files(tmp_path):
    a = input_run(tmp_path / "a")
    (tmp_path / "b").mkdir()
    problems = mac_run.compare_inputs(a, tmp_path / "b")
    assert any("stderr.log missing" in problem for problem in problems)
    assert any("config.toml missing" in problem for problem in problems)


def ipad_args(root, **overrides):
    values = dict(runner="ipad", app=root, bundle_id="org.example.ipad", team="TEAM", maps=None, out=root / "out",
                  xiso=None, simulator=None, exit_after=40.0, time_limit=0.0, set=[], init=[], screenshot_every=0,
                  dump_shaders=False, replay=None, metal_validation=False, metal_shader_validation=False,
                  fresh_config=False)
    values.update(overrides)
    return argparse.Namespace(**values)


def ipad_run_rewrite(tmp_path, monkeypatch, **overrides):
    """what rewrite the iPad path hands to prepare"""
    (tmp_path / "VISR").write_text("")
    seen = []
    monkeypatch.setattr(mac_run, "build_wrapper", lambda args: None)
    monkeypatch.setattr(mac_run, "container_documents", lambda args: tmp_path)
    monkeypatch.setattr(mac_run, "prepare", lambda args, documents, rewrite=False: seen.append(rewrite))
    monkeypatch.setattr(mac_run, "launch", lambda documents: None)
    monkeypatch.setattr(mac_run, "wait_for", lambda condition, limit: True)
    monkeypatch.setattr(mac_run, "collect", lambda documents, out: None)
    mac_run.run(ipad_args(tmp_path, **overrides))
    return seen


def parsed_fresh_config(monkeypatch, argv):
    seen = []
    monkeypatch.setattr(mac_run, "run", lambda args: seen.append(args.fresh_config))
    monkeypatch.setattr("sys.argv", ["mac_run.py", "run", "--out", "out"] + argv)
    mac_run.main()
    return seen[0]


def test_ipad_run_merges_config_without_fresh_config(tmp_path, monkeypatch):
    assert ipad_run_rewrite(tmp_path, monkeypatch) == [False]


def test_ipad_run_rewrites_config_with_fresh_config(tmp_path, monkeypatch):
    assert ipad_run_rewrite(tmp_path, monkeypatch, fresh_config=True) == [True]


def test_fresh_config_flag_and_environment_default(monkeypatch):
    monkeypatch.delenv("HALO_FRESH_CONFIG", raising=False)
    assert parsed_fresh_config(monkeypatch, []) is False
    assert parsed_fresh_config(monkeypatch, ["--fresh-config"]) is True
    monkeypatch.setenv("HALO_FRESH_CONFIG", "1")
    assert parsed_fresh_config(monkeypatch, []) is True
    monkeypatch.setenv("HALO_FRESH_CONFIG", "0")
    assert parsed_fresh_config(monkeypatch, []) is False


def test_runner_option_preserves_explicit_choices_and_leaves_default_unset(monkeypatch):
    seen = []
    monkeypatch.setattr(mac_run, "run", lambda args: seen.append(args.runner))
    for argv in ([], ["--runner", "ipad"], ["--runner", "native"]):
        monkeypatch.setattr("sys.argv", ["mac_run.py", "run", "--out", "out"] + argv)
        mac_run.main()
    assert seen == [None, "ipad", "native"]


def test_simulator_selects_ipad_unless_the_runner_was_explicit(tmp_path, monkeypatch):
    seen = []
    monkeypatch.setattr(mac_run, "run_simulator", lambda args: seen.append(("simulator", args.runner)))
    monkeypatch.setattr(mac_run, "run_native", lambda args: seen.append(("native", args.runner)))

    mac_run.run(native_args(tmp_path, None, simulator="sim-udid", runner=None))
    mac_run.run(native_args(tmp_path, None, simulator=None, runner=None))
    mac_run.run(native_args(tmp_path, None, simulator="sim-udid", runner="ipad"))

    assert seen == [("simulator", "ipad"), ("native", "native"), ("simulator", "ipad")]


def test_simulator_rejects_explicit_native_runner(monkeypatch):
    monkeypatch.setattr(mac_run, "run_simulator", lambda args: pytest.fail("launched incompatible runner"))
    with pytest.raises(SystemExit, match="--simulator runs the simulator app; drop --runner native"):
        mac_run.run(argparse.Namespace(simulator="sim-udid", runner="native"))


def test_native_run_ignores_warnings_but_the_ipad_scheme_keeps_them(tmp_path, monkeypatch, native):
    launches = []
    monkeypatch.setattr(mac_run, "launch_native", fake_launches(launches))
    mac_run.run_native(native_args(tmp_path, native, metal_validation=True))
    environment = launches[0]["environment"]
    assert environment["MTL_DEBUG_LAYER_WARNING_MODE"] == "ignore"
    assert environment["MTL_DEBUG_LAYER"] == "1" and environment["MTL_DEBUG_LAYER_ERROR_MODE"] == "nslog"
    assert mac_run.validation_environment(True, False)["MTL_DEBUG_LAYER_WARNING_MODE"] == "nslog"


def test_the_native_app_comes_from_the_checkout_by_default():
    assert mac_run.native_app_default({}) == mac_run.ROOT / "build/mac/app/Release-maccatalyst/VISR.app"


def test_the_native_app_comes_from_a_host_s_designated_folder(tmp_path):
    assert mac_run.native_app_default({"HALO_MAC_BUILD": str(tmp_path)}) == \
        tmp_path / mac_run.ROOT.name / "app/Release-maccatalyst/VISR.app"


def test_build_wrapper_closes_the_runner_projects_before_xcodegen_rewrites_them(tmp_path, monkeypatch):
    """xcodegen rewrote the open HaloRunner.xcodeproj under Xcode, which answered with a modal
    "changed on disk" alert that no one could click: every later run failed with "Build operations are
    disabled: 'project.xcworkspace' has changed and is reloading" (an M4 build host, 2026-10-05) or waited 300 s,
    and Xcode later aborted in the same handler (an M6 build host, 2026-10-06)"""
    calls = []
    monkeypatch.setattr(mac_run, "RUNNER", tmp_path / "build/mac-runner")
    monkeypatch.setattr(mac_run, "xcode_app", lambda: "/Applications/Xcode.app")
    monkeypatch.setattr(mac_run, "xcode_running", lambda xcode: True)
    def run_command(*args, **options):
        calls.append((args, options.get("input")))
        return mac_run.subprocess.CompletedProcess(args, 0, stdout="0\n", stderr="")
    monkeypatch.setattr(mac_run, "run_command", run_command)
    args = mac_run.argparse.Namespace(team="T", bundle_id="org.example.runner", app=tmp_path / "VISR.app")
    mac_run.build_wrapper(args)
    programs = [call[0][0] for call in calls]
    assert programs.index("osascript") < programs.index("xcodegen")
    close = calls[programs.index("osascript")][1]
    assert 'whose path contains "/HaloRunner.xcodeproj"' in close
    assert "does not start with" not in close


def test_build_wrapper_does_not_start_xcode_to_close_projects(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(mac_run, "RUNNER", tmp_path / "build/mac-runner")
    monkeypatch.setattr(mac_run, "xcode_app", lambda: "/Applications/Xcode.app")
    monkeypatch.setattr(mac_run, "xcode_running", lambda xcode: False)
    monkeypatch.setattr(mac_run, "run_command", lambda *args, **options: calls.append(args[0]))
    args = mac_run.argparse.Namespace(team="T", bundle_id="org.example.runner", app=tmp_path / "VISR.app")
    mac_run.build_wrapper(args)
    assert "osascript" not in calls


def test_close_runner_projects_gives_up_on_an_xcode_held_by_a_dialog():
    """a modal alert keeps Xcode from answering: fail in seconds with the reason, not after 300 s"""
    close = mac_run.CLOSE_RUNNER_PROJECTS.format(xcode="/Applications/Xcode.app", target="HaloRunner")
    assert close.index("with timeout of") < close.index("close")


# trimmed from `sample` of an M4 build host's Xcode, 2026-10-06, held by the alert
HELD_SAMPLE = """Call graph:
    996 Thread_35892   DispatchQueue_1: com.apple.main-thread  (serial)
    + 996 start  (in dyld) + 6688  [0x19c4e7e80]
    +   996 NSApplicationMain  (in AppKit) + 880  [0x1a0effc4c]
    +     996 -[IDEContainer _respondToFileChangeOnDiskWithFilePath:force:]  (in IDEFoundation) + 888  [0x10f988c58]
    +       996 -[IDEDocumentController responseToExternalChangesToBackingFileForContainer:fileWasRemoved:]  (in IDEKit) + 976  [0x10b682268]
    +         996 -[NSAlert runModal]  (in AppKit) + 196  [0x1a11cb2d4]
    996 Thread_35910
    + 996 thread_start  (in libsystem_pthread.dylib) + 8  [0x19c8a6b80]
"""

IDLE_SAMPLE = """Call graph:
    778 Thread_9579811   DispatchQueue_1: com.apple.main-thread  (serial)
    + 778 start  (in dyld) + 6688  [0x196ba1158]
    +   778 -[NSApplication run]  (in AppKit) + 396  [0x1a0f2790c]
    +     778 _DPSNextEvent  (in AppKit) + 580  [0x1a0f344fc]
    778 Thread_9579830
    + 778 -[NSAlert runModal]  (in AppKit) + 196  [0x1a11cb2d4]
"""


def test_modal_alert_reads_the_main_thread_of_a_held_xcode():
    assert mac_run.modal_alert(HELD_SAMPLE) == "a project file that changed on disk"


def test_modal_alert_ignores_an_idle_main_thread_and_other_threads():
    assert mac_run.modal_alert(IDLE_SAMPLE) is None
    assert mac_run.modal_alert("") is None


def test_launch_stops_before_running_when_xcode_is_held(monkeypatch):
    """an M4 build host's Xcode, held by an alert, never loaded the project: LAUNCH spent minutes failing"""
    scripts = []
    monkeypatch.setattr(mac_run, "xcode_app", lambda: "/Applications/Xcode.app")
    monkeypatch.setattr(mac_run, "run_command", lambda *args, **options: scripts.append(options.get("input") or args[0]))
    monkeypatch.setattr(mac_run, "xcode_alert", lambda xcode: "a project file that changed on disk")
    with pytest.raises(SystemExit, match="held by a modal alert"):
        mac_run.launch()
    assert not any("run doc" in script for script in scripts)


def test_wait_for_start_fails_early_when_xcode_is_held(monkeypatch):
    monkeypatch.setattr(mac_run.time, "sleep", lambda seconds: None)
    checks = []

    def held():
        checks.append(1)
        raise SystemExit("held")
    with pytest.raises(SystemExit):
        mac_run.wait_for_start(lambda: False, held, 300, every=0)
    assert checks == [1]


def test_wait_for_start_does_not_check_xcode_once_the_game_runs():
    assert mac_run.wait_for_start(lambda: True, lambda: pytest.fail("checked Xcode"), 300, every=0)


def test_launch_script_does_not_ask_again_over_a_pending_run():
    """a cold Xcode can leave a run "not yet started" for minutes while it builds: failing it, or
    asking again over it, would break a run that was about to start"""
    script = mac_run.LAUNCH.format(xcode="/Applications/Xcode.app", target="HaloRunner", project="/p/HaloRunner.xcodeproj")
    assert 'if run_status is "not yet started"' not in script
    assert 'else if run_status is not "error occurred" then' in script


def test_close_runner_projects_fails_when_a_project_stays_open(monkeypatch):
    """CLOSE_RUNNER_PROJECTS closes inside a try: a close that fails quietly must not let xcodegen
    rewrite a project Xcode still has open"""
    monkeypatch.setattr(mac_run, "xcode_app", lambda: "/Applications/Xcode.app")
    monkeypatch.setattr(mac_run, "xcode_running", lambda xcode: True)
    monkeypatch.setattr(mac_run, "run_command",
                        lambda *args, **options: mac_run.subprocess.CompletedProcess(args, 0, stdout="1\n", stderr=""))
    with pytest.raises(SystemExit, match="still has 1 HaloRunner projects open"):
        mac_run.close_runner_projects()


def test_close_script_reports_the_runner_projects_left_open():
    close = mac_run.CLOSE_RUNNER_PROJECTS.format(xcode="/Applications/Xcode.app", target="HaloRunner")
    assert close.index('return count of (every workspace document whose path contains "/HaloRunner.xcodeproj")') > \
        close.index("close runner saving no")


def _osascript_error(stderr):
    return mac_run.subprocess.CalledProcessError(1, ["osascript"], output="", stderr=stderr)


def test_osascript_failure_names_a_denied_automation_permission(monkeypatch):
    monkeypatch.setattr(mac_run, "xcode_alert", lambda xcode: pytest.fail("sampled Xcode for a permission error"))
    error = _osascript_error("execution error: Not authorized to send Apple events to Xcode-beta. (-1743)")
    message = mac_run.osascript_failure("/Applications/Xcode.app", "closing its HaloRunner projects", error)
    assert "Automation" in message
    assert "alert" not in message


def test_osascript_failure_names_the_alert_that_holds_xcode(monkeypatch):
    monkeypatch.setattr(mac_run, "xcode_alert", lambda xcode: "a project file that changed on disk")
    error = _osascript_error("execution error: Xcode-beta got an error: AppleEvent timed out. (-1712)")
    message = mac_run.osascript_failure("/Applications/Xcode.app", "closing its HaloRunner projects", error)
    assert "held by a modal alert about a project file that changed on disk" in message


def test_osascript_failure_does_not_blame_an_alert_xcode_does_not_have(monkeypatch):
    monkeypatch.setattr(mac_run, "xcode_alert", lambda xcode: None)
    error = _osascript_error("execution error: Xcode-beta got an error: AppleEvent timed out. (-1712)")
    message = mac_run.osascript_failure("/Applications/Xcode.app", "closing its HaloRunner projects", error)
    assert "no alert open" in message


def test_osascript_failure_passes_on_any_other_error(monkeypatch):
    monkeypatch.setattr(mac_run, "xcode_alert", lambda xcode: pytest.fail("sampled Xcode for a script error"))
    error = _osascript_error("execution error: run failed after 3 attempts: Build operations are disabled (-2700)")
    message = mac_run.osascript_failure("/Applications/Xcode.app", "running HaloRunner", error)
    assert "Build operations are disabled" in message


@pytest.fixture
def wrapper(tmp_path, monkeypatch):
    """build_wrapper with Xcode open and xcodegen and xcodebuild faked: returns the programs it ran"""
    calls = []
    monkeypatch.setattr(mac_run, "RUNNER", tmp_path / "build/mac-runner")
    monkeypatch.setattr(mac_run, "xcode_app", lambda: "/Applications/Xcode.app")
    monkeypatch.setattr(mac_run, "xcode_running", lambda xcode: True)

    def run_command(*args, **options):
        calls.append(args[0])
        if args[0] == "xcodegen":
            project = mac_run.RUNNER / "HaloRunner.xcodeproj"
            project.mkdir(parents=True, exist_ok=True)
            (project / "project.pbxproj").write_text("// made by the fake xcodegen\n")
        return mac_run.subprocess.CompletedProcess(args, 0, stdout="0\n", stderr="")
    monkeypatch.setattr(mac_run, "run_command", run_command)
    return calls


def _wrapper_args(tmp_path, **overrides):
    values = dict(team="T", bundle_id="org.example.runner", app=tmp_path / "VISR.app")
    values.update(overrides)
    return mac_run.argparse.Namespace(**values)


def test_build_wrapper_leaves_an_unchanged_project_alone(tmp_path, wrapper):
    """rewriting the project Xcode has open is what raised the "changed on disk" alert"""
    mac_run.build_wrapper(_wrapper_args(tmp_path))
    wrapper.clear()
    mac_run.build_wrapper(_wrapper_args(tmp_path))
    assert "xcodegen" not in wrapper
    assert "osascript" not in wrapper
    assert "xcodebuild" in wrapper


def test_build_wrapper_regenerates_a_changed_project(tmp_path, wrapper):
    mac_run.build_wrapper(_wrapper_args(tmp_path))
    wrapper.clear()
    mac_run.build_wrapper(_wrapper_args(tmp_path, metal_validation=True))
    assert wrapper.index("osascript") < wrapper.index("xcodegen")


def test_build_wrapper_regenerates_when_the_stamp_does_not_match(tmp_path, wrapper):
    mac_run.build_wrapper(_wrapper_args(tmp_path))
    (mac_run.RUNNER / "project.yml.sha256").write_text("stale\n")
    wrapper.clear()
    mac_run.build_wrapper(_wrapper_args(tmp_path))
    assert "xcodegen" in wrapper


def _failing_osascript(stderr, calls=None):
    """run_command where every osascript call fails with stderr"""
    def run_command(*args, **options):
        if calls is not None:
            calls.append(args[0])
        if args[0] == "osascript":
            raise _osascript_error(stderr)
        return mac_run.subprocess.CompletedProcess(args, 0, stdout="", stderr="")
    return run_command


@pytest.fixture
def xcode_open(monkeypatch):
    monkeypatch.setattr(mac_run, "xcode_app", lambda: "/Applications/Xcode.app")
    monkeypatch.setattr(mac_run, "xcode_running", lambda xcode: True)


def test_closing_projects_in_a_held_xcode_names_the_alert(xcode_open, monkeypatch):
    monkeypatch.setattr(mac_run, "run_command", _failing_osascript("AppleEvent timed out. (-1712)"))
    monkeypatch.setattr(mac_run, "xcode_alert", lambda xcode: "a project file that changed on disk")
    with pytest.raises(SystemExit, match="closing its HaloRunner projects: it is held by a modal alert about a project"):
        mac_run.close_runner_projects()


def test_closing_projects_without_automation_permission_says_so(xcode_open, monkeypatch):
    monkeypatch.setattr(mac_run, "run_command", _failing_osascript("Not authorized to send Apple events to Xcode. (-1743)"))
    monkeypatch.setattr(mac_run, "xcode_alert", lambda xcode: None)
    with pytest.raises(SystemExit, match="Automation"):
        mac_run.close_runner_projects()


def test_a_failed_launch_script_exits_with_the_reason_not_a_traceback(xcode_open, monkeypatch):
    """an M4 build host's LAUNCH failed with "Build operations are disabled" inside a CalledProcessError traceback"""
    monkeypatch.setattr(mac_run, "xcode_alert", lambda xcode: None)

    def run_command(*args, **options):
        # CLOSE_OTHERS goes through run_command too: only LAUNCH fails
        if args[0] == "osascript" and "run doc" in options.get("input", ""):
            raise _osascript_error("run failed after 3 attempts: Build operations are disabled: "
                                   "'project.xcworkspace' has changed and is reloading. (-2700)")
        return mac_run.subprocess.CompletedProcess(args, 0, stdout="", stderr="")
    monkeypatch.setattr(mac_run, "run_command", run_command)
    with pytest.raises(SystemExit, match="while running HaloRunner: run failed after 3 attempts: Build operations"):
        mac_run.launch()


def test_xcode_alert_samples_xcode_s_main_thread(monkeypatch):
    monkeypatch.setattr(mac_run, "xcode_pid", lambda xcode: 2748)

    def sample(command, **options):
        assert command[:3] == ["sample", "2748", "1"]
        Path(command[command.index("-file") + 1]).write_text(HELD_SAMPLE)
        return mac_run.subprocess.CompletedProcess(command, 0)
    monkeypatch.setattr(mac_run.subprocess, "run", sample)
    assert mac_run.xcode_alert("/Applications/Xcode.app") == "a project file that changed on disk"


def test_xcode_alert_is_none_when_sample_writes_nothing(monkeypatch):
    monkeypatch.setattr(mac_run, "xcode_pid", lambda xcode: 2748)
    monkeypatch.setattr(mac_run.subprocess, "run", lambda command, **options: mac_run.subprocess.CompletedProcess(command, 1))
    assert mac_run.xcode_alert("/Applications/Xcode.app") is None


def test_chosen_origins_marks_what_a_run_writes():
    """every key a run writes into config.toml is marked chosen in config.origins, so a device profile never
    moves a runner's value; the game's own lines for other keys are kept"""
    old = "display.render_height\tdefault\t0\ninput.turn\tdefault\t\"snap\"\ndisplay.renderer\tchosen\n"
    text = mac_run.chosen_origins(old, {"display.render_height", "display.shadow_map_size"})
    assert text.splitlines() == ['input.turn\tdefault\t"snap"', "display.renderer\tchosen",
                                 "display.render_height\tchosen", "display.shadow_map_size\tchosen"]
    assert mac_run.chosen_origins("", {"display.vsync"}) == "display.vsync\tchosen\n"
    assert mac_run.chosen_origins(text, set()) == text


class _Opener:
    def __init__(self):
        self.killed = False

    def wait(self, timeout=None):
        return 0

    def kill(self):
        self.killed = True


def test_a_native_app_that_ignores_term_is_killed(tmp_path):
    calls = []

    def run(command, **options):
        calls.append(command)
        # pgrep keeps finding the app: it ignored TERM
        return argparse.Namespace(returncode=0)

    mac_run.end_native(tmp_path / "VISR.app", _Opener(), grace=3, run=run, sleep=lambda seconds: None)
    assert calls[0][:2] == ["pkill", "-f"] and calls[-1][:3] == ["pkill", "-KILL", "-f"]
    assert sum(1 for call in calls if call[0] == "pgrep") == 3


def test_a_native_app_that_ends_on_term_is_not_killed(tmp_path):
    calls = []

    def run(command, **options):
        calls.append(command)
        return argparse.Namespace(returncode=1 if command[0] == "pgrep" else 0)

    mac_run.end_native(tmp_path / "VISR.app", _Opener(), grace=3, run=run, sleep=lambda seconds: None)
    assert [call[0] for call in calls] == ["pkill", "pgrep"] and "-KILL" not in calls[0]
def test_defaults_are_settings_the_game_has():
    """every setting the runner resets is one of config.toml's
    (port/linux/src/port_config.c)"""
    import re
    root = Path(__file__).resolve().parents[1]
    config = (root / "port/linux/src/port_config.c").read_text()
    known = set(re.findall(r'^\t\{ "([a-z_0-9]+\.[a-z_0-9]+)", _config_', config, re.M))
    assert sorted(set(mac_run.DEFAULTS) - known) == []


def test_defaults_reset_the_network_addresses():
    """a two-instance run's addresses and internet play do not leak into the next run"""
    assert mac_run.DEFAULTS["network.address"] == '""'
    assert mac_run.DEFAULTS["network.broadcast"] == '""'
    assert mac_run.DEFAULTS["network.online"] == "false"
