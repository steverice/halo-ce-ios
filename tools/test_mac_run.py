"""Tests for the Mac runner's pure helpers (tools/mac_run.py)."""

import plistlib
import struct
from pathlib import Path

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
    wanted = _container(tmp_path, "B", "org.steverice.haloce.macrunner")
    assert mac_run.find_container(tmp_path, "org.steverice.haloce.macrunner") == wanted / "Data/Documents"


def test_find_container_returns_none_when_absent(tmp_path):
    _container(tmp_path, "A", "org.example.other")
    assert mac_run.find_container(tmp_path, "org.steverice.haloce.macrunner") is None


def _bmp(width: int, height: int, pixels: bytes) -> bytes:
    """a 32-bit top-down BMP laid out as write_screenshot (d3d8_gl.c) writes it"""
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
    assert mac_run.bmp_difference(image, image) == (0, 0)


def test_bmp_difference_counts_pixels_and_largest_delta_ignoring_alpha():
    a = _bmp(2, 1, b"\x10\x10\x10\xff\x20\x20\x20\xff")
    b = _bmp(2, 1, b"\x10\x10\x10\x00\x20\x2a\x20\xff")
    assert mac_run.bmp_difference(a, b) == (1, 10)


def test_bmp_difference_different_sizes_counts_every_pixel():
    a = _bmp(2, 1, bytes(8))
    b = _bmp(1, 1, bytes(4))
    assert mac_run.bmp_difference(a, b) == (2, 255)


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
    script = mac_run.LAUNCH.format(xcode="/Applications/Xcode.app", target="HaloRunner")
    wait = script.index('exists (first workspace document whose path contains "mac-runner/HaloRunner.xcodeproj")')
    assert wait < script.index("set doc to")


def test_reset_settings_turn_off_the_fixed_timestep(tmp_path):
    """a run that doesn't ask for the virtual clock must get the real one"""
    settings = mac_run.reset_settings(tmp_path, screenshot_every=0, dump_shaders=False, replay=False)
    assert settings["debug.fixed_timestep"] == "false"


def test_launch_script_fails_when_the_ipad_destination_never_appears():
    """after the retries, a last unguarded attempt raises instead of running on another destination"""
    script = mac_run.LAUNCH.format(xcode="/Applications/Xcode.app", target="HaloRunner")
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
