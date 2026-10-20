#!/usr/bin/env python3
"""Run the game on this Mac, and compare runs. The native Mac Catalyst runner is the default;
`run --runner ipad` runs the iPad build as a "Designed for iPad" app instead.

For the iPad runner: macOS launches an iPad app only if Xcode installed it. It also kills the app
that port/ios's CMake project builds ("Code Signature Invalid"), although the
same executable runs when a plain Xcode project signs it. So `run` generates a
small wrapper project around the CMake-built VISR executable and Info.plist,
installs and launches it through Xcode with the debugger off (memory_watch.c
write-protects pages and expects their faults, which would stop a debugger),
waits for the game to quit (debug.exit_after), and copies the logs,
screenshots and shader files out of the app's container. `compare` checks two
such result folders against each other.

`run --simulator UDID` runs a simulator build (an iOS or visionOS simulator app,
tools/ios_build.py --simulator) in that simulator instead, with the same
config.toml, init.txt and result folders, so its runs compare with the Mac's.

`run` (`--runner native`, the default) runs the Mac Catalyst build (tools/ios_build.py --mac): no Xcode,
no device registration, started with `open` on a persistent data folder per host, with the
display pinned to what the iPad runner sees, so its results compare with the iPad runner's.
"""

import argparse
import datetime
import difflib
import hashlib
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# a debug.gpu_stats summary (d3d8_device.c), after platform_log's prefix
STATS = re.compile(r"frame \d+: (.*)$")
# the GL call total at the end of a debug.gpu_stats summary
GL_CALLS = re.compile(r",? \d+ GL calls$")
# what a backend reports it can do (gpu_gl.c, gpu_metal.m)
CAPABILITIES = re.compile(r"GPU capabilities: (.*)$")
# the capabilities Metal has and Apple's OpenGL ES lacks, so a GL run and a Metal run report them differently:
# they decide how the front end uploads vertex colors and indices (d3d8_device.c), not what is drawn
BACKEND_CAPABILITIES = re.compile(r"(?:vertex BGRA|base vertex) \d+(?:, )?")
# the upload totals in a debug.gpu_stats summary, which follow those capabilities
UPLOAD_BYTES = re.compile(r"\d+ KB mirrored, \d+ KB streamed(?:, )?")
# a translated shader's source (.glsl or .metal) and the inputs it was translated from
SHADER_SOURCES = ("*.glsl", "*.metal")
SHADER_INPUTS = ("*.vsh", "*.key")


def coalesce_tables(lines):
    """config.toml lines with each table's repeated headers folded into its first: TOML rejects a table
    defined twice, and the game then ignores the whole file"""
    preamble, order, bodies, current = [], [], {}, None
    for line in lines:
        if line.startswith("[") and line.rstrip().endswith("]"):
            current = line.strip()
            if current not in bodies:
                order.append(current)
                bodies[current] = []
            continue
        (preamble if current is None else bodies[current]).append(line)
    # a key set twice in one table is invalid too; the first (the one merge_config updates) wins
    out = list(preamble)
    for header in order:
        out.append(header)
        keys = set()
        for line in bodies[header]:
            match = re.match(r"^([A-Za-z0-9_]+)\s*=", line)
            if match:
                if match.group(1) in keys:
                    continue
                keys.add(match.group(1))
            out.append(line)
    return out


def merge_config(text, settings):
    """config.toml text with each dotted key in settings set to its raw TOML value"""
    lines = coalesce_tables(text.splitlines())
    for dotted, value in settings.items():
        section, key = dotted.split(".", 1)
        header = f"[{section}]"
        if header not in lines:
            lines += [header, f"{key} = {value}"]
            continue
        start = lines.index(header)
        end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("[")), len(lines))
        pattern = re.compile(rf"^{re.escape(key)}\s*=")
        for index in range(start + 1, end):
            if pattern.match(lines[index]):
                lines[index] = f"{key} = {value}"
                break
        else:
            insert = end
            while insert > start + 1 and not lines[insert - 1].strip():
                insert -= 1
            lines.insert(insert, f"{key} = {value}")
    return "\n".join(lines) + "\n"


def chosen_origins(old, keys):
    """config.origins text (port/linux/src/port_config.c) with each dotted key in keys marked chosen: a value a
    run writes into config.toml is in force whatever a device profile says, including one equal to a default,
    which the game would otherwise take for a default it wrote and let the profile move"""
    kept = [line for line in old.splitlines() if line.split("\t", 1)[0] not in keys]
    return "".join(f"{line}\n" for line in kept + [f"{key}\tchosen" for key in sorted(keys)])


# what port_config.c defaults these to: a run that doesn't --set one gets
# the default, not whatever an earlier run's --set left in config.toml
DEFAULTS = {
    "display.screen_width": "0",
    "display.render_height": "0",
    "display.vsync": "true",
    "display.renderer": '"gl"',
    "display.interpolation": "true",
    "display.anisotropic_filtering": "1",
    "display.shadow_map_size": "128",
    "display.effect_resolution": "false",
    "display.compressed_textures": "false",
    "display.frame_pacing": '"off"',
    "display.frame_rate": '"display"',
    # (a runner run sets debug.exit_after, so "auto" compiles at each first draw, as before)
    "display.async_shaders": '"auto"',
    "display.movies": '"auto"',
    "display.render_scale": "1.0",
    "display.upscaler": '"bilinear"',
    "display.native_hud": "true",
    "display.high_res_hud": "true",
    "display.high_res_text": "true",
    "display.upscaled_textures": "true",
    "display.anti_aliasing": '"off"',  # the recorded references have no smoothing pass
    "display.immersive": "false",
    "display.mirror_resolution": '"half"',
    "display.model_lod": '"auto"',  # the recorded references were made with the original LOD
    "display.lod_scale": "1.0",
    "display.theater_width": "60.0",
    "display.theater_distance": "4.0",
    "display.theater_environment": '"passthrough"',
    "display.stereo": '"off"',
    "display.frame_repeat": "0",
    "display.film_depth_share": "0.25",
    "display.film_convergence": "1.75",
    "display.screen_depth_share": "0.3",
    "display.screen_convergence": "1.0",
    "display.screen_framing": '"band"',
    "display.stereo_vehicle_screen": "false",
    "display.hud_corner_across": "28.0",
    "display.hud_corner_up": "20.0",
    "display.hud_tracker_down": "22.0",
    "display.hud_messages_up": "12.0",
    "display.hud_scale": "1.0",
    "display.hud_distance": "2.0",
    "display.hud_depth": "true",
    "display.hud_depth_share": "0.85",
    "display.hud_depth_floor": "0.3",
    "display.hud_depth_pull_in": "0.1",
    "display.hud_depth_relax": "1.0",
    "display.hud_depth_relax_delay": "0.5",
    "display.hud_resolution": "1.0",
    "display.foveation": "true",
    "display.render_quality": "0.6",
    "display.first_person_body": "true",
    "display.first_person_legs": "false",
    "display.first_person_legs_seat_shift": "1.0",
    "display.first_person_body_offset": "0.08",
    "display.first_person_body_seats": "true",
    "display.first_person_body_seat_offset": "0.0",
    "display.weapon_offset_down": "0.0",
    "display.weapon_offset_back": "0.0",
    "display.eye_height_offset": "0.0",
    "display.seat_look_fade": "true",
    "display.seat_look_fade_start": "5.0",
    "display.seat_look_fade_black": "45.0",
    "display.seat_look_fade_blur": "true",
    "display.pod_look_fade_start": "15.0",
    "display.pod_look_fade_end": "70.0",
    "display.pod_look_fade_dim": "0.85",
    "input.turn": '"snap"',
    "input.snap_angle": "30.0",
    "input.smooth_turn_speed": "120.0",
    "input.comfort_vignette": "false",
    "network.address": '""',
    "network.broadcast": '""',
    "network.online": "false",
    "network.online_ask": "false",
    "network.lan_broadcast": '"auto"',
    "debug.link_consent_answer": '""',
    "input.stick_dead_zone": "0.27",
    "debug.null_renderer": "false",
    "debug.gl_debug": "false",
    "debug.render_validation": '"log"',
    "debug.render_validation_allow": '""',
    "debug.render_capture": '""',
    "debug.render_capture_frames": '""',
    "debug.metal_state_cache": "true",
    "debug.metal_specialize": "true",
    "debug.metal_vertex_bgra": "true",
    "debug.metal_base_vertex": "true",
    "debug.metal_pipeline_archive": "false",
    "debug.shader_list_warm": "true",
    "debug.shader_list_record": '"shader-lists-missed"',
    "debug.fixed_timestep": "false",
    "debug.side_by_side_screen": "false",
    "debug.screen_lean": "0.0",
    "debug.side_by_side_tangents": '""',
    "debug.input_record": '""',
    "debug.input_replay": '""',
    "debug.benchmark": "false",
    "debug.benchmark_lip_sync": "false",
    "debug.benchmark_suite": '""',
    "debug.gpu_stats": "false",
    "debug.gpu_trace_frame": "-1",
    "debug.gpu_trace_constants": "false",
    "debug.gpu_skip_vertex_shaders": '""',
    "debug.gpu_shader_replay_dialect": '""',
    "debug.gpu_debug_expression": '""',
    "debug.gpu_debug_texture0": "false",
    "debug.gpu_debug_flat": "false",
    "debug.texture_dump_directory": '""',
    "debug.texture_override_directory": '""',
    "debug.texture_log": "false",
    "debug.texture_no_cache": "false",
    "debug.terminal_on_screen": "false",
    "debug.render_scale_dpad": "false",
    "debug.sense_tuning_chord": "false",
    "debug.foveation_eye_passes": "true",
    "debug.rate_map_test": "false",
    "debug.network_test": '""',
    "debug.network_test_start": "15.0",
    "debug.network_test_players": "1",
    "debug.network_test_kill": "0.0",
    "debug.network_test_score": "0",
    "debug.network_test_shoot": "0.0",
    "debug.network_test_vehicle": "0.0",
    "debug.network_test_pickup": "0.0",
    "debug.network_test_pickup_weapon": '""',
    "debug.network_latency": "0.0",
    "debug.network_loss": "0.0",
    # ("auto" is off in a runner run, which sets debug.exit_after)
    "game.loading_screen": '"auto"',
    "game.loading_screen_seconds": "1.5",
    "debug.telnet_console": "false",
    "debug.telnet_console_port": "2323",
    "debug.test_input": '""',
    "debug.update_answer": '""',
    "debug.hidden_window": "false",
    "debug.fixed_timestep_paced": "false",
    "debug.frame_trace": "false",
    "debug.frame_counter": "false",
    "debug.test_extra_scene": "false",
    "debug.test_theater_reopen": "false",
    "debug.gpu_flush_draws": "-1",
}


def reset_settings(documents, screenshot_every, dump_shaders, replay):
    """the settings of a run before its --set ones: requested outputs go into
    runner/, the rest are off, and everything else is at its default"""
    runner = Path(documents) / "runner"
    return {
        **DEFAULTS,
        "debug.screenshot_every": str(screenshot_every),
        "debug.screenshot_directory": f'"{runner}/shots"' if screenshot_every else '""',
        "debug.gpu_dump_shaders": f'"{runner}/shaders"' if dump_shaders else '""',
        "debug.gpu_shader_replay": f'"{runner}/replay"' if replay else '""',
    }


def find_container(root, bundle_id):
    """the Data/Documents folder of the container macOS made for bundle_id, or None"""
    for metadata in sorted(Path(root).glob("*/.com.apple.containermanagerd.metadata.plist")):
        try:
            with metadata.open("rb") as file:
                identifier = plistlib.load(file).get("MCMMetadataIdentifier")
        except (OSError, plistlib.InvalidFileException):
            continue
        if identifier == bundle_id:
            return metadata.parent / "Data/Documents"
    return None


def read_bmp(data):
    """(width, height, BGRA rows) of a 32-bit BMP as write_screenshot (d3d8_device.c) writes it"""
    if len(data) < 30 or data[:2] != b"BM":
        raise ValueError("not a BMP")
    (offset,) = struct.unpack_from("<I", data, 10)
    width, height = struct.unpack_from("<ii", data, 18)
    (bits,) = struct.unpack_from("<H", data, 28)
    if bits != 32:
        raise ValueError(f"{bits}-bit BMP; expected 32")
    height = abs(height)
    if len(data) < offset + width * height * 4:
        raise ValueError(f"truncated: {len(data)} bytes")
    return width, height, data[offset:offset + width * height * 4]


def region_pixels(width, height, region):
    """the pixel box (left, top, right, bottom) of region, fractions (x0, y0, x1, y1) of a
    width x height frame from its top left; the whole frame without one"""
    if region is None:
        return 0, 0, width, height
    x0, y0, x1, y1 = region
    return (round(x0 * width), round(y0 * height), round(x1 * width), round(y1 * height))


def bmp_difference(a, b, channel_tolerance=0, region=None):
    """(pixels whose color differs by more than channel_tolerance in some channel,
    largest channel difference, (x, y) of the first pixel that difference is at, or
    None); alpha is ignored, since write_screenshot forces it opaque. With region
    (fractions x0, y0, x1, y1 from the top left: write_screenshot's rows are top
    down), only the pixels inside it count"""
    width_a, height_a, pixels_a = read_bmp(a)
    width_b, height_b, pixels_b = read_bmp(b)
    if (width_a, height_a) != (width_b, height_b):
        return max(width_a * height_a, width_b * height_b), 255, None
    if pixels_a == pixels_b:
        return 0, 0, None
    left, top, right, bottom = region_pixels(width_a, height_a, region)
    differing = largest = 0
    where = None
    row = width_a * 4
    for start in range(top * row, bottom * row, row):
        line_a, line_b = pixels_a[start:start + row], pixels_b[start:start + row]
        if line_a == line_b:
            continue
        for pixel in range(left * 4, right * 4, 4):
            delta = max(abs(line_a[pixel + channel] - line_b[pixel + channel]) for channel in range(3))
            if delta > channel_tolerance:
                differing += 1
            if delta > largest:
                largest, where = delta, (pixel // 4, start // row)
    return differing, largest, where


def bmp_pixels(data, region=None):
    """the pixel count of a BMP, or of region in it (region_pixels)"""
    width, height, _ = read_bmp(data)
    left, top, right, bottom = region_pixels(width, height, region)
    return max(right - left, 0) * max(bottom - top, 0)


def parse_region(text):
    """--region's X0,Y0,X1,Y1: fractions of a frame from its top left, with X0 < X1 and Y0 < Y1"""
    try:
        values = tuple(float(part) for part in text.split(","))
    except ValueError:
        values = ()
    if len(values) != 4 or not all(0.0 <= value <= 1.0 for value in values) or \
            values[0] >= values[2] or values[1] >= values[3]:
        raise argparse.ArgumentTypeError(f"{text!r} is not X0,Y0,X1,Y1 with 0 <= X0 < X1 <= 1 and 0 <= Y0 < Y1 <= 1")
    return values


def stats_lines(log):
    """the debug.gpu_stats summaries in a log, without their frame numbers"""
    return [match.group(1) for match in map(STATS.search, log.splitlines()) if match]


def _files(folder, pattern):
    return {path.name: path for path in folder.glob(pattern)} if folder.is_dir() else {}


def _shader_files(folder, patterns):
    files = {}
    for pattern in patterns:
        files.update(_files(folder, pattern))
    return files


def _compare_files(problems, folder, a, b, files_a, files_b, contents):
    for name in sorted(files_a.keys() ^ files_b.keys()):
        problems.append(f"{folder}/{name} only in {a if name in files_a else b}")
    if contents:
        for name in sorted(files_a.keys() & files_b.keys()):
            if files_a[name].read_bytes() != files_b[name].read_bytes():
                problems.append(f"{folder}/{name} differs")


def capability_lines(log):
    """the GPU capability lines in a log"""
    return [match.group(1) for match in map(CAPABILITIES.search, log.splitlines()) if match]


# The log lines that name a run's inputs and environment, which the iPad and native runners must print
# identically (the native runner's parity runs): the guest image, the display the guest is told, the
# render size, the GL implementation and the entry points it lacks, the
# capabilities, the audio device, and a shader replay's verdict. "display pinned" is the native runner's
# own and is not compared.
INPUT_LINES = re.compile(
    r"(guest image sha256 [0-9a-f]{64}"
    r"|guest display: .*"
    r"|screen: .* drawn at .*"
    r"|OpenGL ES \d+\.\d+: .*"
    r"|OpenGL function \S+ is unavailable"
    r"|OpenGL .* on .*"
    r"|Metal on .*"
    r"|GPU capabilities: .*"
    r"|audio device: .*"
    r"|audio output active: .*"
    r"|shader replay: .*)$")
# the lines every game run's log must have, under either runner and either renderer: a pair of logs that
# both lack one would otherwise compare equal on that point
REQUIRED_INPUTS = {
    "the guest's SHA-256": re.compile(r"^guest image sha256 "),
    "the guest's display": re.compile(r"^guest display: "),
    "the renderer": re.compile(r"^(OpenGL .* on |Metal on )"),
    "the GPU capabilities": re.compile(r"^GPU capabilities: "),
    "the audio device": re.compile(r"^audio device: "),
}
# a path into a run's own data folder in config.toml, which differs between the runners
RUNNER_PATH = re.compile(r'^"[^"]*/runner(/|")')


def input_lines(log):
    """the lines of a log that name the run's inputs (INPUT_LINES), in order"""
    return [match.group(1) for match in map(INPUT_LINES.search, log.splitlines()) if match]


def config_settings(text):
    """{"section.key": raw value} of a config.toml as prepare writes it, with paths into the run's
    data folder written as $DATA, so two runners' files compare by their settings"""
    settings, section = {}, ""
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            continue
        key, _, value = line.partition("=")
        settings[f"{section}.{key.strip()}"] = RUNNER_PATH.sub(r'"$DATA/runner\1', value.strip())
    return settings


def compare_inputs(a, b):
    """the differences between what two result folders' runs were given (empty: none): the
    INPUT_LINES of their logs, in order, and their config.toml settings"""
    a, b = Path(a), Path(b)
    problems = [f"{folder / name} missing" for folder in (a, b) for name in ("stderr.log", "config.toml")
                if not (folder / name).is_file()]
    if problems:
        return problems
    lines_a, lines_b = (input_lines((folder / "stderr.log").read_text(errors="replace")) for folder in (a, b))
    for folder, lines in ((a, lines_a), (b, lines_b)):
        for what, pattern in REQUIRED_INPUTS.items():
            if not any(pattern.search(line) for line in lines):
                problems.append(f"{folder}/stderr.log does not name {what}")
    if not lines_a and not lines_b:
        problems.append(f"{a}/stderr.log and {b}/stderr.log name none of the run's inputs")
    elif lines_a != lines_b:
        diff = difflib.unified_diff(lines_a, lines_b, str(a), str(b), lineterm="", n=0)
        problems.append("log lines differ:\n  " + "\n  ".join(list(diff)[2:]))
    config_a, config_b = (config_settings((folder / "config.toml").read_text()) for folder in (a, b))
    for key in sorted(config_a.keys() | config_b.keys()):
        if config_a.get(key) != config_b.get(key):
            problems.append(f"config.toml {key}: {config_a.get(key)} against {config_b.get(key)}")
    return problems


def compare(a, b, tolerance=0, ignore_gl_calls=False, channel_tolerance=0, fraction=0.0, across_backends=False,
            region=None):
    """the differences between two result folders (empty: they match). A pixel
    differs when a channel differs by more than channel_tolerance, and a frame
    matches when no more than tolerance pixels, or fraction of its pixels,
    differ. With region (fractions x0, y0, x1, y1 of each frame from its top
    left) only the pixels inside it are compared, and fraction is of those. With ignore_gl_calls the gpu_stats lines are compared without their
    GL call totals. across_backends compares a GL run with a Metal run: the
    shaders' inputs (.vsh, .key) must be the same files and their sources the
    same names, whatever their language; GL calls and the upload totals
    (mirrored, streamed) are ignored; and the two backends must report the
    same capabilities, apart from vertex BGRA and base vertex, which Metal has
    and Apple's OpenGL ES lacks."""
    a, b = Path(a), Path(b)
    problems = []
    for folder in ("runner/shaders", "runner/replay/replay"):
        if across_backends:
            stems_a = {Path(name).stem: path for name, path in _shader_files(a / folder, SHADER_SOURCES).items()}
            stems_b = {Path(name).stem: path for name, path in _shader_files(b / folder, SHADER_SOURCES).items()}
            _compare_files(problems, folder, a, b, stems_a, stems_b, contents=False)
            _compare_files(problems, folder, a, b, _shader_files(a / folder, SHADER_INPUTS),
                           _shader_files(b / folder, SHADER_INPUTS), contents=True)
        else:
            _compare_files(problems, folder, a, b, _shader_files(a / folder, SHADER_SOURCES),
                           _shader_files(b / folder, SHADER_SOURCES), contents=True)
    shots_a, shots_b = _files(a / "runner/shots", "*.bmp"), _files(b / "runner/shots", "*.bmp")
    for name in sorted(shots_a.keys() ^ shots_b.keys()):
        problems.append(f"{name} only in {a if name in shots_a else b}")
    for name in sorted(shots_a.keys() & shots_b.keys()):
        try:
            data_a = shots_a[name].read_bytes()
            differing, largest, where = bmp_difference(data_a, shots_b[name].read_bytes(), channel_tolerance, region)
            allowed = max(tolerance, int(fraction * bmp_pixels(data_a, region)))
        except ValueError as error:
            problems.append(f"{name}: unreadable ({error})")
            continue
        if differing > allowed:
            at = f" at {where[0]},{where[1]}" if where else ""
            problems.append(f"{name}: {differing} pixels differ, by up to {largest}{at}")
    logs = [folder / "stderr.log" for folder in (a, b)]
    missing = [str(log) for log in logs if not log.is_file()]
    if missing:
        problems += [f"{log} missing" for log in missing]
    else:
        texts = [log.read_text(errors="replace") for log in logs]
        stats_a, stats_b = (stats_lines(text) for text in texts)
        if across_backends:
            capabilities_a, capabilities_b = (capability_lines(text) for text in texts)
            if not capabilities_a or [BACKEND_CAPABILITIES.sub("", line) for line in capabilities_a] != \
                    [BACKEND_CAPABILITIES.sub("", line) for line in capabilities_b]:
                problems.append(f"capabilities differ:\n  {a}: {capabilities_a}\n  {b}: {capabilities_b}")
        if ignore_gl_calls or across_backends:
            stats_a, stats_b = ([GL_CALLS.sub("", line) for line in lines] for lines in (stats_a, stats_b))
        if across_backends:
            stats_a, stats_b = ([UPLOAD_BYTES.sub("", line) for line in lines] for lines in (stats_a, stats_b))
        if stats_a != stats_b:
            problems.append(f"gpu_stats differ:\n  {a}: {stats_a}\n  {b}: {stats_b}")
    return problems


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "build/mac-runner"
TARGET = "HaloRunner"
CONTAINERS = Path.home() / "Library/Containers"
DESTINATION = "platform=macOS,arch=arm64,variant=Designed for iPad"

# The run scheme turns off Xcode's GPU validation and frame capture. A scheme
# that doesn't say leaves Metal's API validation on, even with the debugger off:
# every Metal run got an MTLDebugDevice, whose checks cost most of a frame's CPU.
# --metal-validation turns validation on through the environment instead.
PROJECT = """name: {target}
options:
  bundleIdPrefix: org.steverice.visr
targets:
  {target}:
    type: application
    platform: iOS
    deploymentTarget: "16.0"
    sources: [stub.c]
    settings:
      DEVELOPMENT_TEAM: {team}
      PRODUCT_BUNDLE_IDENTIFIER: {bundle_id}
      GENERATE_INFOPLIST_FILE: YES
      CURRENT_PROJECT_VERSION: "1"
      MARKETING_VERSION: "1.0"
      ENABLE_DEBUG_DYLIB: NO
    postBuildScripts:
      - name: Use the CMake-built app
        basedOnDependencyAnalysis: false
        script: |
          cp "{app}/{executable}" "$TARGET_BUILD_DIR/$EXECUTABLE_PATH"
          cp "{app}/Info.plist" "$TARGET_BUILD_DIR/$INFOPLIST_PATH"
          plutil -replace CFBundleExecutable -string "$EXECUTABLE_NAME" "$TARGET_BUILD_DIR/$INFOPLIST_PATH"
          plutil -replace CFBundleIdentifier -string "$PRODUCT_BUNDLE_IDENTIFIER" "$TARGET_BUILD_DIR/$INFOPLIST_PATH"
schemes:
  {target}:
    build:
      targets:
        {target}: all
    run:
      config: Debug
      debugEnabled: false
      enableGPUValidationMode: disabled
      enableGPUFrameCaptureMode: disabled
{environment}"""

# Metal's API validation (gpu_metal.m), reported to the logs rather than
# stopping the app, so a run still reaches its screenshots
METAL_VALIDATION = {
    "MTL_DEBUG_LAYER": "1",
    "MTL_DEBUG_LAYER_ERROR_MODE": "nslog",
    "MTL_DEBUG_LAYER_WARNING_MODE": "nslog",
}
# shader validation on top: it checks every memory access the shaders make,
# which slows a10 past the run's time limit, and it reports the game's own
# NaN texture coordinates, which GL passes on silently
METAL_SHADER_VALIDATION = {
    "MTL_SHADER_VALIDATION": "1",
    "MTL_SHADER_VALIDATION_REPORT_TO_STDERR": "1",
}


def validation_environment(metal_validation, metal_shader_validation):
    """the scheme's environment for the validation a run asks for"""
    environment = {}
    if metal_validation or metal_shader_validation:
        environment.update(METAL_VALIDATION)
    if metal_shader_validation:
        environment.update(METAL_SHADER_VALIDATION)
    return environment


def scheme_environment(variables):
    """the run scheme's environmentVariables block for PROJECT ("" for none)"""
    if not variables:
        return ""
    return "      environmentVariables:\n" + "".join(f'        {name}: "{value}"\n' for name, value in variables.items())

# open -a returns before a cold Xcode has the project open, and a freshly
# loaded project lists its run destinations a little later still
# Xcode keeps one project of a name open: another checkout's runner project would
# block this one from opening, and be driven in its place. Match by path, not
# name: Xcode can list another runner project as ".../HaloRunner.xcodeproj/
# project.xcworkspace", named "project.xcworkspace", and one whose worktree was
# deleted stays open that way, leaving this project's run "not yet started" for
# good. Close each on its own, since such a stale one can fail to answer.
CLOSE_OTHERS = """tell application "{xcode}"
	repeat with other in (every workspace document whose path contains "/{target}.xcodeproj" and path does not start with "{project}")
		try
			close other saving no
		end try
	end repeat
end tell
"""

# xcodegen rewrites HaloRunner.xcodeproj whenever its inputs change (regenerate_project).
# With the project open, Xcode reloads it, and sometimes answers with a modal "changed
# on disk" alert instead, which no one is there to click: every later run then fails
# with "Build operations are disabled: 'project.xcworkspace' has changed and is
# reloading" or stays "not yet started" (an M4 build host, 2026-10-05), and Xcode later aborted
# in that same handler (an M6 build host, 2026-10-06). So every runner project is closed before
# xcodegen runs, and LAUNCH opens this one again. An Xcode held by such an alert does
# not answer: give up in seconds, with the reason (osascript_failure)
CLOSE_RUNNER_PROJECTS = """with timeout of 30 seconds
	tell application "{xcode}"
		repeat with runner in (every workspace document whose path contains "/{target}.xcodeproj")
			try
				close runner saving no
			end try
		end repeat
		-- a close can fail quietly (inside the try): report what is still open
		return count of (every workspace document whose path contains "/{target}.xcodeproj")
	end tell
end timeout
"""

LAUNCH = """tell application "{xcode}"
	repeat 120 times
		if exists (first workspace document whose path is "{project}") then exit repeat
		delay 1
	end repeat
	set doc to first workspace document whose path is "{project}"
	repeat 120 times
		if loaded of doc then exit repeat
		delay 1
	end repeat
	repeat 60 times
		try
			set active run destination of doc to (first run destination of doc whose name is "My Mac (Designed for iPad)")
			exit repeat
		end try
		delay 1
	end repeat
	-- unguarded: a destination that never appeared fails here, not later
	set active run destination of doc to (first run destination of doc whose name is "My Mac (Designed for iPad)")
	-- right after opening or reloading the project Xcode sometimes answers "Cannot
	-- run", or is still loading it: wait for it, and try again
	set last_error to "Xcode did not report a run"
	set started_run to false
	repeat 3 times
		repeat 120 times
			if loaded of doc then exit repeat
			delay 1
		end repeat
		try
			set result_of_run to run doc
			repeat 120 times
				if (status of result_of_run as text) is not "not yet started" then exit repeat
				delay 1
			end repeat
			set run_status to (status of result_of_run as text)
			-- xcodegen rewrites the open project before each run; when Xcode reloads it
			-- only after the run was asked for, the reload cancels the run before it
			-- launches anything (seen on an M4 Mac mini): ask again
			if run_status is "cancelled" then
				set last_error to "Xcode cancelled the run"
			-- still pending after two minutes (a cold Xcode can still be building or
			-- indexing): don't ask again over a pending run; mac_run.py keeps waiting for the
			-- game and checks Xcode for a modal alert meanwhile
			else if run_status is not "error occurred" then
				set started_run to true
				exit repeat
			else
				set last_error to (error message of result_of_run as text)
			end if
		on error message_of_error
			set last_error to message_of_error
		end try
		delay 5
	end repeat
	if not started_run then error "run failed after 3 attempts: " & last_error
end tell
"""


def run_command(*args, **options):
    print("+", " ".join(str(arg) for arg in args), flush=True)
    return subprocess.run([str(arg) for arg in args], check=True, **options)


def xcode_app():
    developer = subprocess.check_output(["xcode-select", "--print-path"], text=True).strip()
    return str(Path(developer).parents[1])


def xcode_pid(xcode):
    pids = subprocess.run(["pgrep", "-f", f"{xcode}/Contents/MacOS/Xcode"], capture_output=True, text=True).stdout.split()
    return int(pids[0]) if pids else None


def xcode_running(xcode):
    return xcode_pid(xcode) is not None


def modal_alert(sample_text):
    """what holds Xcode's main thread in a modal alert, from `sample`'s output; None when nothing does.
    Neither AppleScript nor the window list (no titles without Screen Recording) tells an alert apart,
    but the main thread's stack does"""
    lines = sample_text.splitlines()
    start = next((i for i, line in enumerate(lines) if "com.apple.main-thread" in line), None)
    if start is None:
        return None
    main = []
    for line in lines[start + 1:]:
        if re.match(r"^\s*\d+ Thread_", line) or not line.strip():
            break
        main.append(line)
    main = "\n".join(main)
    if "runModal" not in main and "_doModalLoop" not in main:
        return None
    if "responseToExternalChangesToBackingFileForContainer" in main:
        return "a project file that changed on disk"
    if "presentError" in main:
        return "an error"
    return "something unknown"


def xcode_alert(xcode):
    """the modal alert holding Xcode, if any: a 1-second `sample` of its main thread (about 2 s)"""
    pid = xcode_pid(xcode)
    if pid is None:
        return None
    with tempfile.TemporaryDirectory() as folder:
        report = Path(folder) / "sample.txt"
        subprocess.run(["sample", str(pid), "1", "-file", report], capture_output=True)
        return modal_alert(report.read_text(errors="replace")) if report.exists() else None


def osascript_failure(xcode, doing, error):
    """why an osascript call to Xcode failed, from its error number: a denied Automation permission,
    an Xcode that stopped answering (held by an alert, or busy), or the script's own error"""
    message = (error.stderr or "").strip()
    if "(-1743)" in message:
        return (f"this Mac does not let the process running mac_run.py (Terminal, or sshd-keygen-wrapper over ssh) "
                f"control Xcode, so it failed while {doing}: allow it under System Settings > Privacy & Security > "
                f"Automation on this Mac's screen ({message})")
    if "(-1712)" in message:
        alert = xcode_alert(xcode)
        if alert:
            return (f"Xcode ({xcode}) did not answer while {doing}: it is held by a modal alert about {alert} "
                    "on this Mac's screen; answer the alert (or quit Xcode), then run again")
        return f"Xcode ({xcode}) did not answer in time while {doing}, with no alert open: it may be busy ({message})"
    return f"osascript failed while {doing}: {message or f'exit status {error.returncode}'}"


def exit_if_xcode_alert(xcode):
    alert = xcode_alert(xcode)
    if alert:
        sys.exit(f"Xcode ({xcode}) is held by a modal alert about {alert} on this Mac's screen, so it can't "
                 f"load or run {TARGET}: answer the alert (or quit Xcode), then run again")


def close_runner_projects():
    """close every runner project in Xcode before xcodegen rewrites this one (see CLOSE_RUNNER_PROJECTS)"""
    xcode = xcode_app()
    if not xcode_running(xcode):
        return
    try:
        closed = run_command("osascript", input=CLOSE_RUNNER_PROJECTS.format(xcode=xcode, target=TARGET), text=True,
                             capture_output=True)
    except subprocess.CalledProcessError as error:
        sys.exit(osascript_failure(xcode, f"closing its {TARGET} projects", error))
    still_open = closed.stdout.strip()
    if still_open != "0":
        sys.exit(f"Xcode ({xcode}) still has {still_open or 'an unknown number of'} {TARGET} projects open after "
                 f"closing them, and xcodegen would rewrite one under it; close them in Xcode, then run again")


def running():
    return subprocess.run(["pgrep", "-x", TARGET], capture_output=True).returncode == 0


def started(documents, is_running):
    """whether the launched game is running or has run: a short run (debug.fixed_timestep
    makes one take seconds) can start and quit between two polls, leaving only its log"""
    return is_running() or (documents is not None and (documents / "ios-runtime.log").exists())


def wait_for(condition, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(2)
    return False


def app_executable(app):
    """the CMake-built device app's executable name, from its Info.plist: VISR, or HaloCE in an app built before
    the VISR rename (VISR when the plist can't be read)"""
    try:
        with (Path(app) / "Info.plist").open("rb") as file:
            return plistlib.load(file).get("CFBundleExecutable") or "VISR"
    except (OSError, plistlib.InvalidFileException):
        return "VISR"


def build_wrapper(args):
    RUNNER.mkdir(parents=True, exist_ok=True)
    (RUNNER / "stub.c").write_text("int main(void) { return 0; }\n")
    (RUNNER / "project.yml").write_text(PROJECT.format(
        target=TARGET, team=args.team, bundle_id=args.bundle_id, app=args.app.resolve(),
        executable=app_executable(args.app),
        environment=scheme_environment(validation_environment(getattr(args, "metal_validation", False),
                                                             getattr(args, "metal_shader_validation", False)))))
    regenerate_project()
    run_command("xcodebuild", "-project", RUNNER / f"{TARGET}.xcodeproj", "-scheme", TARGET,
                "-destination", DESTINATION, "-allowProvisioningUpdates", "build",
                stdout=subprocess.DEVNULL)


def project_inputs_hash():
    """what xcodegen makes HaloRunner.xcodeproj from: the spec, the stub and the xcodegen it runs"""
    xcodegen = shutil.which("xcodegen")
    digest = hashlib.sha256()
    for part in ((RUNNER / "project.yml").read_bytes(), (RUNNER / "stub.c").read_bytes(),
                 str(Path(xcodegen).resolve() if xcodegen else "").encode()):
        digest.update(hashlib.sha256(part).digest())
    return digest.hexdigest()


def regenerate_project():
    """run xcodegen only when its inputs changed since the last run, so an unchanged project stays as
    Xcode has it open; when they did, close the runner projects first (CLOSE_RUNNER_PROJECTS)"""
    stamp = RUNNER / "project.yml.sha256"
    inputs = project_inputs_hash()
    project = RUNNER / f"{TARGET}.xcodeproj/project.pbxproj"
    if project.is_file() and stamp.is_file() and stamp.read_text().strip() == inputs:
        print(f"{TARGET}.xcodeproj is up to date: not regenerating it", flush=True)
        return
    stamp.unlink(missing_ok=True)
    close_runner_projects()
    run_command("xcodegen", "generate", "--spec", RUNNER / "project.yml", "--project", RUNNER, "--quiet")
    stamp.write_text(inputs + "\n")


def launch(documents=None):
    """install and start the wrapper through Xcode (the only way macOS accepts); documents
    is the container's Documents folder, once one exists"""
    xcode = xcode_app()
    project = RUNNER / f"{TARGET}.xcodeproj"
    run_command("osascript", input=CLOSE_OTHERS.format(xcode=xcode, target=TARGET, project=project), text=True)
    run_command("open", "-a", xcode, project)
    exit_if_xcode_alert(xcode)
    try:
        run_command("osascript", input=LAUNCH.format(xcode=xcode, target=TARGET, project=project), text=True,
                    stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as error:
        sys.exit(osascript_failure(xcode, f"running {TARGET}", error))
    if not wait_for_start(lambda: started(documents, running), lambda: exit_if_xcode_alert(xcode), 300):
        sys.exit(f"{TARGET} did not start within 300 seconds; see Xcode's report navigator")


def wait_for_start(has_started, check_xcode, seconds, every=30):
    """wait_for, checking Xcode for a modal alert every so often, so a held Xcode fails the run in
    seconds rather than at the deadline"""
    deadline = time.monotonic() + seconds
    next_check = time.monotonic() + every
    while time.monotonic() < deadline:
        if has_started():
            return True
        if time.monotonic() >= next_check:
            check_xcode()
            next_check = time.monotonic() + every
        time.sleep(2)
    return False


def container_documents(args):
    documents = find_container(CONTAINERS, args.bundle_id)
    if documents:
        return documents
    print("first run of this bundle id: launching once so macOS creates its container", flush=True)
    launch()
    if not wait_for(lambda: find_container(CONTAINERS, args.bundle_id), 120):
        sys.exit(f"no container appeared for {args.bundle_id}")
    subprocess.run(["pkill", "-x", TARGET])
    wait_for(lambda: not running(), 30)
    return find_container(CONTAINERS, args.bundle_id)


def prepare(args, documents, rewrite=False):
    documents.mkdir(parents=True, exist_ok=True)
    if args.xiso and not (documents / "maps").is_dir():
        run_command("cp", "-c", args.xiso, documents / args.xiso.name)
    runner = documents / "runner"
    shutil.rmtree(runner, ignore_errors=True)
    runner.mkdir()
    # the game writes into these folders but does not create them
    if args.screenshot_every:
        (runner / "shots").mkdir()
    if args.dump_shaders:
        (runner / "shaders").mkdir()
    if getattr(args, "record_shader_lists", False):
        (runner / "shader-lists").mkdir()
    if args.replay:
        shutil.copytree(args.replay, runner / "replay",
                        ignore=lambda folder, names: [n for n in names if not n.endswith((".vsh", ".key"))])
    settings = reset_settings(documents, args.screenshot_every, args.dump_shaders, bool(args.replay))
    if getattr(args, "record_shader_lists", False):
        settings["debug.shader_list_record"] = f'"{runner}/shader-lists"'
    settings["debug.exit_after"] = f"{float(args.exit_after)}"
    if getattr(args, "benchmark_suite", None):
        # the device benchmark's suite ends the game itself when its last run is over
        settings["debug.benchmark_suite"] = f'"{args.benchmark_suite}"'
        settings["debug.exit_after"] = "0.0"
        shutil.rmtree(documents / "benchmark", ignore_errors=True)
    for assignment in args.set:
        key, value = assignment.split("=", 1)
        settings[key.strip()] = value.strip()
    # Explicit controller replays opt into the benchmark takes' simulated mouths. Shader replay is unrelated.
    if settings["debug.input_replay"].strip('"'):
        if not any(item.split("=", 1)[0].strip() == "debug.benchmark_lip_sync" for item in args.set):
            settings["debug.benchmark_lip_sync"] = "true"
    config = documents / "config.toml"
    # rewrite: from DEFAULTS and this run's settings only (the native runner's persistent data
    # folder); otherwise merged into the container's file, as the iPad runner always has
    old = "" if rewrite or not config.is_file() else config.read_text()
    config.write_text(merge_config(old, settings))
    # every value this run wrote is the run's, not a default for the device's profile to move
    origins = documents / "config.origins"
    origins.write_text(chosen_origins("" if rewrite or not origins.is_file() else origins.read_text(), set(settings)))
    init = documents / "init.txt"
    if args.init:
        init.write_text("\n".join(args.init) + "\n")
    elif init.exists():
        init.unlink()
    for name in ("stderr.log", "debug.txt", "ios-runtime.log"):
        (documents / name).unlink(missing_ok=True)
    (documents / "stderr.log").touch()


# a benchmark suite without --time-limit: an hour and a half of wall time, then it is killed
SUITE_LIMIT = 5400


def collect(documents, out):
    out.mkdir(parents=True, exist_ok=True)
    for name in ("ios-runtime.log", "stderr.log", "debug.txt", "config.toml", "config.origins"):
        if (documents / name).is_file():
            shutil.copy2(documents / name, out / name)
    if (documents / "runner").is_dir():
        shutil.copytree(documents / "runner", out / "runner", dirs_exist_ok=True)
    # automatic takes (debug.input_record = "auto"), newest last
    if (documents / "recordings").is_dir():
        shutil.copytree(documents / "recordings", out / "recordings", dirs_exist_ok=True)
    # the device benchmark's journal, runs and report (--benchmark-suite), without the recordings it replayed
    if (documents / "benchmark").is_dir():
        (out / "benchmark").mkdir(exist_ok=True)
        for file in sorted((documents / "benchmark").iterdir()):
            if file.is_file():
                shutil.copy2(file, out / "benchmark" / file.name)


def simulator_pattern(udid):
    """a pgrep -f pattern matching the game's executable in simulator udid"""
    return f"CoreSimulator/Devices/{udid}/.*/VISR.app/VISR"


def simulator_running(udid):
    """the game is running in simulator udid"""
    return subprocess.run(["pgrep", "-f", simulator_pattern(udid)], capture_output=True).returncode == 0


def run_simulator(args):
    udid = args.simulator
    app = args.app or ROOT / "build/visionos/app-simulator/Release-xrsimulator/VISR.app"
    if not (app / "VISR").is_file():
        sys.exit(f"no simulator app at {app}; run tools/ios_build.py --simulator (--visionos) first")
    with (app / "Info.plist").open("rb") as file:
        info = plistlib.load(file)
    bundle_id = info["CFBundleIdentifier"]
    # boots the simulator if it isn't, and waits until it has
    run_command("xcrun", "simctl", "bootstatus", udid, "-b", stdout=subprocess.DEVNULL)
    subprocess.run(["xcrun", "simctl", "terminate", udid, bundle_id], capture_output=True)
    run_command("xcrun", "simctl", "install", udid, app)
    container = subprocess.run(["xcrun", "simctl", "get_app_container", udid, bundle_id, "data"],
                               capture_output=True, text=True, check=True).stdout.strip()
    # an Apple TV app may write only to Caches, and keeps everything in Caches/Halo (host_main.m)
    documents = Path(container) / ("Library/Caches/Halo" if info.get("DTPlatformName") == "appletvsimulator" else "Documents")
    if args.maps and not (documents / "maps").is_dir():
        documents.mkdir(parents=True, exist_ok=True)
        run_command("cp", "-c", "-R", args.maps, documents / "maps")
    prepare(args, documents)
    run_command("xcrun", "simctl", "launch", udid, bundle_id, stdout=subprocess.DEVNULL)
    if not wait_for(lambda: simulator_running(udid), 60):
        collect(documents, args.out)
        sys.exit(f"the game did not start in simulator {udid} within 60 seconds; logs are in {args.out}")
    limit = args.time_limit or (SUITE_LIMIT if getattr(args, "benchmark_suite", None) else args.exit_after + 120)
    finished = wait_for(lambda: not simulator_running(udid), limit)
    if not finished:
        subprocess.run(["xcrun", "simctl", "terminate", udid, bundle_id], capture_output=True)
    collect(documents, args.out)
    if not finished:
        sys.exit(f"the game was still running {limit} seconds after launch and was stopped; logs are in {args.out}")
    print(f"results: {args.out}")


def native_app_default(environment=None):
    """the Catalyst app tools/ios_build.py --mac built: in the checkout, or under $HALO_MAC_BUILD (a host
    that runs developer-built apps only from one folder)"""
    environment = os.environ if environment is None else environment
    if environment.get("HALO_MAC_BUILD"):
        return Path(environment["HALO_MAC_BUILD"]) / ROOT.name / "app/Release-maccatalyst/VISR.app"
    return ROOT / "build/mac/app/Release-maccatalyst/VISR.app"


NATIVE_EXECUTABLE = "Contents/MacOS/VISR"
# what the iPad runner's SDL reports on a 2x screen, pinned for the native app (host_main.m)
NATIVE_DISPLAY = "1366x1024@2"
# the game's exit (host_exit, host_main.m) in ios-runtime.log: open --wait-apps returns 0 whatever it was
GAME_EXIT = re.compile(r"game exit (-?\d+)$")
# characters a pgrep pattern (extended regular expression) treats specially
ERE_SPECIAL = re.compile(r"([.^$*+?()\[\]{}|\\])")


def native_data_folder(bundle_id, environment=None, home=None):
    """the native runner's data folder (one per host, kept between runs): $HALO_NATIVE_DATA (a remote
    host's, set by its job runner), else ~/Library/Application Support/BUNDLE_ID/runner-data"""
    environment = os.environ if environment is None else environment
    if environment.get("HALO_NATIVE_DATA"):
        return Path(environment["HALO_NATIVE_DATA"])
    return (home or Path.home()) / "Library/Application Support" / bundle_id / "runner-data"


def native_command(app, data, out, environment):
    """the open command that starts the native app on data, pinned, with environment for the game"""
    variables = {"HALO_DATA_ROOT": str(data), "HALO_HOST_DISPLAY": NATIVE_DISPLAY, "HALO_RUNNER": "1",
                 **environment}
    command = ["open", "--new", "--wait-apps"]
    for name, value in variables.items():
        command += ["--env", f"{name}={value}"]
    # open's own stdout and stderr go to files in the data folder, not to out: launchd opens them, and
    # macOS's TCC denies its helper (xpcproxy) access to removable volumes, so a path on one (a
    # baselines symlink to an external drive) fails the launch with -10810 before the app starts. The
    # data folder is on the internal disk on every host; launch_native copies the logs into out afterward
    return command + ["--stdout", str(data / "open-stdout.log"), "--stderr", str(data / "open-stderr.log"),
                      str(app)]


def native_pattern(app):
    """a pgrep -f pattern matching the native app's executable"""
    return ERE_SPECIAL.sub(r"\\\1", f"{Path(app).resolve()}/{NATIVE_EXECUTABLE}")


def native_pids(app):
    """the processes running the native app's executable"""
    result = subprocess.run(["pgrep", "-f", native_pattern(app)], capture_output=True, text=True)
    return [int(pid) for pid in result.stdout.split()]


def game_exit(log):
    """the status the game exited with, from its log, or None if it never got to exit"""
    for line in reversed(log.splitlines()):
        match = GAME_EXIT.search(line)
        if match:
            return int(match.group(1))
    return None


OPEN_LOGS = ("open-stdout.log", "open-stderr.log")


def launch_native(app, data, out, environment, limit):
    """start the native app with open and wait for it to quit: True when it did within limit
    seconds, False when it was still running and was killed"""
    out.mkdir(parents=True, exist_ok=True)
    data.mkdir(parents=True, exist_ok=True)
    for name in OPEN_LOGS:
        (data / name).unlink(missing_ok=True)
    try:
        return _launch_native(app, data, out, environment, limit)
    finally:
        for name in OPEN_LOGS:
            if (data / name).is_file():
                shutil.copy2(data / name, out / name)


# how long a native app is given to end after TERM before it is killed outright
TERM_GRACE = 15


def end_native(app, opener, grace=TERM_GRACE, run=subprocess.run, sleep=time.sleep):
    """ends a native app that outlived its time limit: TERM, then KILL if it is still there after grace seconds.
    A game spinning in its own code has ignored TERM (a level change under debug.fixed_timestep, 2026-10-13: the
    run then held its host's line for an hour), so TERM alone is not an end"""
    pattern = native_pattern(app)
    run(["pkill", "-f", pattern])
    for _ in range(grace):
        if run(["pgrep", "-f", pattern], capture_output=True).returncode != 0:
            break
        sleep(1)
    else:
        print(f"the native app ignored TERM for {grace} s: killing it", flush=True)
        run(["pkill", "-KILL", "-f", pattern])
    try:
        opener.wait(timeout=30)
    except subprocess.TimeoutExpired:
        opener.kill()


def _launch_native(app, data, out, environment, limit):
    command = native_command(app, data, out, environment)
    print("+", " ".join(command), flush=True)
    opener = subprocess.Popen(command)
    try:
        status = opener.wait(timeout=limit)
    except subprocess.TimeoutExpired:
        end_native(app, opener)
        return False
    if status != 0:
        sys.exit(f"open could not start {app} (exit {status}): a user must be logged in to this Mac's "
                 f"screen; see {out}/open-stderr.log")
    return True


SEED_MARKER = "seeded"


def seeding_run(args, data, label, out, init, exit_after, unseed=True):
    """play one throwaway run of a data folder's seed: the menu or a10, fixed timestep, no
    screenshots. When open fails or the game does not exit cleanly, exit naming the logs. For a
    folder this runner cloned (unseed), first rename maps back to maps.partial so the next run
    clones again rather than trusting the folder; maps that were already there are left alone. No
    marker is written either way, so the next run seeds again."""
    settings = argparse.Namespace(xiso=None, screenshot_every=0, dump_shaders=False, replay=None,
                                  exit_after=exit_after, set=["debug.fixed_timestep=true"], init=init)
    prepare(settings, data, rewrite=True)
    try:
        finished = launch_native(args.app, data, out, {}, 600)
    except SystemExit:
        # open itself failed: unseed here too, or the next run would trust the folder
        if unseed:
            (data / "maps").rename(data / "maps.partial")
        raise
    collect(data, out)
    log = out / "ios-runtime.log"
    if not finished or (game_exit(log.read_text(errors="replace")) if log.is_file() else None) != 0:
        if unseed:
            (data / "maps").rename(data / "maps.partial")
        sys.exit(f"the throwaway {label} run in the data folder {data} failed; its logs are in {out}")


def seed_native_data(args, data):
    """seed a data folder that has no `seeded` marker: play two throwaway runs, then write the marker.
    The a10 run comes first: a host's first a10 GL run in a newly seeded folder has fallen behind on
    two of the build hosts. The menu run comes second: the first menu run in a new folder
    writes last_language.dat and savegame.bin and precaches the ui map once more, so it differs from
    every later menu run, and the iPad runner's container is always past that state. A folder without
    maps is cloned from --maps first (into maps.partial, renamed when complete, so an interrupted
    clone is redone); a failed throwaway run then unseeds it. A folder that already has maps (made some
    other way, such as by an earlier Catalyst spike) is not cloned into and its maps are never renamed
    or removed, so --maps is not needed; a failed throwaway run just exits. The marker is written only
    after both runs succeed."""
    cloned = not (data / "maps").is_dir()
    if cloned:
        if not args.maps or not Path(args.maps).is_dir():
            sys.exit(f"no maps in {data}: pass --maps with a folder of extracted maps to seed it from")
        data.mkdir(parents=True, exist_ok=True)
        partial = data / "maps.partial"
        shutil.rmtree(partial, ignore_errors=True)
        run_command("cp", "-c", "-R", Path(args.maps), partial)
        partial.rename(data / "maps")
        print(f"seeded {data}: cloned the maps, then a throwaway a10 run and a throwaway menu run (the "
              f"first menu run in a new folder writes the language and savegame files and precaches ui "
              f"once more)", flush=True)
    else:
        print(f"seeded {data}: it has maps but no {SEED_MARKER} marker, so no clone; a throwaway a10 run "
              f"and a throwaway menu run (the first a10 run in such a folder would precache a10, and the "
              f"first menu run writes the language and savegame files and precaches ui once more)",
              flush=True)
    seeding_run(args, data, "a10", args.out.parent / f"{args.out.name}-seed", ["map_name a10"], 40.0, cloned)
    seeding_run(args, data, "menu", args.out.parent / f"{args.out.name}-seed-menu", [], 20.0, cloned)
    note = f"{datetime.date.today().isoformat()}: throwaway a10 run, then throwaway menu run"
    (data / SEED_MARKER).write_text(note + "\n")


def seed_native_movies(args, data):
    """clone the intro and credits movies into the data folder when --maps has a bink folder beside it
    (an import's Documents) and the data folder has none; display.movies "auto" still skips them in
    a timed run"""
    movies = Path(args.maps).parent / "bink" if args.maps else None
    if movies and movies.is_dir() and not (data / "bink").exists():
        run_command("cp", "-c", "-R", movies, data / "bink")


def run_native(args):
    args.app = args.app or native_app_default()
    if not (args.app / NATIVE_EXECUTABLE).is_file():
        sys.exit(f"no native app at {args.app}; run tools/ios_build.py --mac first")
    with (args.app / "Contents/Info.plist").open("rb") as file:
        bundle_id = plistlib.load(file)["CFBundleIdentifier"]
    if args.bundle_id and args.bundle_id != bundle_id:
        sys.exit(f"{args.app} was built for {bundle_id}, not {args.bundle_id}; "
                 f"rebuild it with tools/ios_build.py --mac --bundle-id {args.bundle_id}")
    pids = native_pids(args.app)
    if pids:
        sys.exit(f"{args.app} is already running (pid {', '.join(map(str, pids))}); a second copy would "
                 f"share its data folder. Stop it, or wait for it to finish")
    data = native_data_folder(bundle_id)
    if not (data / SEED_MARKER).is_file():
        seed_native_data(args, data)
    seed_native_movies(args, data)
    prepare(args, data, rewrite=True)
    limit = args.time_limit or (SUITE_LIMIT if getattr(args, "benchmark_suite", None) else args.exit_after + 120)
    environment = validation_environment(args.metal_validation, args.metal_shader_validation)
    for assignment in getattr(args, "app_env", None) or []:
        name, _, value = assignment.partition("=")
        environment[name] = value
    if "MTL_DEBUG_LAYER_WARNING_MODE" in environment:
        # an open-launched app's NSLog lands in stderr.log, which check-metal greps for validation errors,
        # while the Xcode-launched iPad runner's goes to the unified log. Both runners get the same warnings
        # (unused bindings, redundant sets), so ignore them here (Metal's default): any validation line
        # left in the native stderr.log is then an error
        environment["MTL_DEBUG_LAYER_WARNING_MODE"] = "ignore"
    finished = launch_native(args.app, data, args.out, environment, limit)
    collect(data, args.out)
    if not finished:
        sys.exit(f"the native app was still running {limit} seconds after launch and was killed; "
                 f"logs are in {args.out}")
    log = args.out / "ios-runtime.log"
    status = game_exit(log.read_text(errors="replace")) if log.is_file() else None
    if status != 0:
        sys.exit(f"the game did not exit cleanly (game exit {status}); logs are in {args.out}")
    print(f"results: {args.out}")


def run(args):
    if args.simulator:
        if args.runner == "native":
            sys.exit("--simulator runs the simulator app; drop --runner native")
        if args.runner is None:
            args.runner = "ipad"
        run_simulator(args)
        return
    if args.runner is None:
        args.runner = "native"
    if args.runner == "native":
        if args.team or args.xiso:
            sys.exit("--runner native needs no --team, and seeds its data folder from --maps, not --xiso")
        run_native(args)
        return
    args.bundle_id = args.bundle_id or "org.steverice.visr.macrunner"
    if not args.team:
        sys.exit("--runner ipad needs --team")
    args.app = args.app or ROOT / "build/ios/app-device/Release-iphoneos/VISR.app"
    if not (args.app / app_executable(args.app)).is_file():
        sys.exit(f"no CMake-built app at {args.app}; run tools/ios_build.py --team ... first")
    build_wrapper(args)
    documents = container_documents(args)
    prepare(args, documents, rewrite=args.fresh_config)
    launch(documents)
    limit = args.time_limit or (SUITE_LIMIT if getattr(args, "benchmark_suite", None) else args.exit_after + 120)
    finished = wait_for(lambda: not running(), limit)
    if not finished:
        subprocess.run(["pkill", "-x", TARGET])
    collect(documents, args.out)
    if not finished:
        sys.exit(f"{TARGET} was still running {limit} seconds after launch and was killed; "
                 f"logs are in {args.out}")
    print(f"results: {args.out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    run_parser = commands.add_parser("run", help="run the game once and collect its results")
    run_parser.add_argument("--team", help="Apple development team ID (--runner ipad only)")
    run_parser.add_argument("--runner", choices=("ipad", "native"), default=None,
                            help="native: the Mac Catalyst app (tools/ios_build.py --mac), started with open "
                                 "(the default without --simulator); "
                                 'ipad: the "Designed for iPad" app, installed and started by Xcode, which needs --team')
    run_parser.add_argument("--bundle-id",
                            help="ipad: the runner's bundle ID (default org.steverice.visr.macrunner); "
                                 "native: checked against the app's own")
    run_parser.add_argument("--app", type=Path,
                            help="the CMake-built device app (tools/ios_build.py --team ...; the default), or "
                                 "with --simulator the simulator app (default: the visionOS one; an Apple TV "
                                 "simulator takes build/tvos/app-simulator/Release-appletvsimulator/VISR.app)")
    run_parser.add_argument("--simulator", metavar="UDID",
                            help="run the simulator app in this simulator (xcrun simctl list devices)")
    run_parser.add_argument("--maps", type=Path,
                            help="a folder of extracted maps: with --simulator, copied into a new container; "
                                 "with --runner native, cloned into a new data folder, in place of importing --xiso")
    run_parser.add_argument("--out", type=Path, required=True, help="folder to copy the results to")
    run_parser.add_argument("--xiso", type=Path, help="the player's XISO, imported on the first run")
    run_parser.add_argument("--exit-after", type=float, default=60.0,
                            help="seconds before the game quits; of game time (30 frames each) "
                                 "with debug.fixed_timestep")
    run_parser.add_argument("--time-limit", type=float, default=0.0,
                            help="seconds after launch before the run is killed (default --exit-after + 120); "
                                 "with debug.fixed_timestep --exit-after counts seconds of game time, 30 frames each "
                                 "(600 = 18,000 frames), which validation slows")
    run_parser.add_argument("--set", action="append", default=[], metavar="SECTION.KEY=VALUE",
                            help="a config.toml setting, value in TOML syntax (repeatable)")
    run_parser.add_argument("--app-env", action="append", default=[], metavar="NAME=VALUE",
                            help="native runner: a variable for the app's own environment (not a setting)")
    run_parser.add_argument("--benchmark-suite", metavar="MODE",
                            help="run the device benchmark's suite (debug.benchmark_suite) in place of a timed run: "
                                 "the game quits when the suite is over (--exit-after is ignored; --time-limit bounds "
                                 "it, 90 minutes by default) and its journal, runs and report.json land in OUT/benchmark")
    run_parser.add_argument("--init", action="append", default=[], metavar="COMMAND",
                            help="a console command for init.txt, e.g. 'map_name a10' (repeatable)")
    run_parser.add_argument("--screenshot-every", type=int, default=0, metavar="FRAMES")
    run_parser.add_argument("--dump-shaders", action="store_true")
    run_parser.add_argument("--record-shader-lists", action="store_true",
                            help="append the shaders and pipelines made while drawing to runner/shader-lists/MAP.txt "
                                 "(tools/shader_lists.py merges them into port/shader-lists)")
    run_parser.add_argument("--replay", type=Path, help="a folder of recorded .vsh/.key shader inputs")
    run_parser.add_argument("--metal-validation", action="store_true",
                            help="turn on Metal's API validation (for display.renderer=\"metal\" runs)")
    run_parser.add_argument("--metal-shader-validation", action="store_true",
                            help="turn on Metal's shader validation too (slow: a10 needs a longer --time-limit)")
    run_parser.add_argument("--fresh-config", action="store_true", default=os.environ.get("HALO_FRESH_CONFIG") == "1",
                            help="iPad runner: write config.toml from the defaults and this run's settings instead of "
                                 "merging into the container's file (parity: a container shared with other branches "
                                 "carries their settings); the native runner always does. "
                                 "HALO_FRESH_CONFIG=1 turns it on")
    compare_parser = commands.add_parser("compare", help="compare two result folders")
    compare_parser.add_argument("a", type=Path)
    compare_parser.add_argument("b", type=Path)
    compare_parser.add_argument("--tolerance", type=int, default=0, help="pixels a frame may differ by")
    compare_parser.add_argument("--channel-tolerance", type=int, default=0, metavar="LEVELS",
                                help="channel difference a pixel may have and still match (0-255)")
    compare_parser.add_argument("--fraction", type=float, default=0.0,
                                help="fraction of a frame's pixels that may differ (overrides a smaller --tolerance)")
    compare_parser.add_argument("--ignore-gl-calls", action="store_true",
                                help="compare gpu_stats without the GL call totals")
    compare_parser.add_argument("--across-backends", action="store_true",
                                help="a GL run against a Metal run: shader inputs, not sources; capabilities; no GL calls")
    compare_parser.add_argument("--region", type=parse_region, metavar="X0,Y0,X1,Y1",
                                help="compare only this part of each frame, as fractions from its top left")
    inputs_parser = commands.add_parser("compare-inputs",
                                        help="compare what two runs were given: the log lines that name their "
                                             "inputs, and config.toml (the native runner's parity runs)")
    inputs_parser.add_argument("a", type=Path)
    inputs_parser.add_argument("b", type=Path)
    args = parser.parse_args()
    if args.command == "run":
        run(args)
    elif args.command == "compare-inputs":
        problems = compare_inputs(args.a, args.b)
        print("\n".join(problems) if problems else "match")
        sys.exit(1 if problems else 0)
    else:
        problems = compare(args.a, args.b, args.tolerance, args.ignore_gl_calls, args.channel_tolerance,
                           args.fraction, args.across_backends, args.region)
        print("\n".join(problems) if problems else "match")
        sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
