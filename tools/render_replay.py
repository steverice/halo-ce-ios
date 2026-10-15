#!/usr/bin/env python3
"""Replay render captures on this Mac without the game, and check what they draw.

A render capture (port/ios/host/gpu_capture.h) is a window of frames as the calls the game made into the GPU
backend, written by a build with render validation (tools/ios_build.py --mac --render-validation on) when
debug.render_capture names a folder and debug.render_capture_frames the windows. This tool starts the same app
with a capture instead of the game: it draws the frames into the backend the capture was made on, saves them,
and exits, in a second or two.

    render_replay.py info CAPTURE...            what each file holds
    render_replay.py replay CAPTURE --out DIR   replay one file: frames (BMP), frames.txt, result.txt, logs
    render_replay.py check SET [--record]       replay every capture of a set and compare with the set's records
    render_replay.py fuzz CAPTURE --seeds N     replay with calls replaced by wrong ones; the validator must refuse each
    render_replay.py across DIR_A DIR_B         compare two replays' frames within a tolerance (GL against Metal)

A set is a folder of captures beside the baselines, never in the repository (captures hold the game's own
textures and models):

    SET/NAME.capture                    e.g. a10-metal-00600.capture
    SET/expected/NAME/frames.txt        each frame's size and hash, as this Mac's GPU drew it
    SET/expected/NAME/frameNNNNN.bmp    the window's last frame

`check --record` writes the expected folders; `check` compares against them and exits 1 on any difference, any
validation failure, or a capture the build refuses as stale. Records belong to the GPU that drew them: keep a
set per host, as the GL records are kept.
"""

import argparse
import gzip
import os
import plistlib
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mac_run  # noqa: E402  (the native runner's launch and comparison helpers)

HEADER = struct.Struct("<8sIII6II20sII68s64s")
MAGIC = b"VISRCAP\0"


def read_header(path):
    """a capture's header as a dictionary; ValueError if it isn't one"""
    with gzip.open(path, "rb") as file:
        data = file.read(HEADER.size)
    if len(data) < HEADER.size:
        raise ValueError("too short to be a render capture")
    fields = HEADER.unpack(data)
    if fields[0] != MAGIC:
        raise ValueError("not a render capture")
    capabilities = fields[11]
    return {
        "format": fields[1], "interface_hash": fields[2], "front_end_hash": fields[3], "draw_size": fields[4],
        "initialize_flags": fields[10], "backend": "metal" if struct.unpack_from("<H", capabilities, 8)[0] == 1 else "gl",
        "first_frame": fields[12], "frame_count": fields[13],
        "guest_sha256": fields[14].split(b"\0")[0].decode(errors="replace"),
        "label": fields[15].split(b"\0")[0].decode(errors="replace"),
    }


def build_hashes(root=mac_run.ROOT):
    """this checkout's (interface hash, front end hash), as tools/gpu_interface_hash.py computes them"""
    import gpu_interface_hash
    return (gpu_interface_hash.source_hash(gpu_interface_hash.INTERFACE, root),
            gpu_interface_hash.source_hash(gpu_interface_hash.FRONT_END, root))


def staleness(header, hashes):
    """why a capture is stale for this checkout: "interface" (gpu.h changed: it cannot be replayed), "front end"
    (the calls a frame makes may have changed: it replays, but no longer covers the front end), or None"""
    if header["interface_hash"] != hashes[0]:
        return "interface"
    if header["front_end_hash"] != hashes[1]:
        return "front end"
    return None


def read_result(folder):
    """result.txt as a dictionary (numbers as int), {} if the replay never wrote it"""
    result = {}
    path = Path(folder) / "result.txt"
    if path.is_file():
        for line in path.read_text(errors="replace").splitlines():
            key, _, value = line.partition(" ")
            result[key] = int(value) if value.lstrip("-").isdigit() else value
    return result


def app_bundle_id(app):
    with (app / "Contents/Info.plist").open("rb") as file:
        return plistlib.load(file)["CFBundleIdentifier"]


def replay(app, capture, out, validation="log", fuzz=None, shots="all", force=False, limit=180):
    """replay one capture in the app; returns (result dictionary, seconds from launch to exit, game exit status or
    None if the app died without exiting)"""
    out = Path(out)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    # a data folder of its own beside the runner's: no game data is read, and the logs land here
    data = mac_run.native_data_folder(app_bundle_id(app)).parent / "replay-data"
    shutil.rmtree(data, ignore_errors=True)
    data.mkdir(parents=True)
    (data / "stderr.log").write_text("")
    environment = {
        "HALO_RENDER_REPLAY": str(Path(capture).resolve()), "HALO_RENDER_REPLAY_OUT": str(data / "render-replay"),
        "HALO_RENDER_REPLAY_SHOTS": shots, "HALO_RENDER_VALIDATION": validation,
        "MTL_DEBUG_LAYER_WARNING_MODE": "ignore",
    }
    if fuzz:
        environment["HALO_RENDER_REPLAY_FUZZ"] = fuzz
    if force:
        environment["HALO_RENDER_REPLAY_FORCE"] = "1"
    started = time.monotonic()
    finished = mac_run.launch_native(app, data, out, environment, limit)
    seconds = time.monotonic() - started
    for name in ("stderr.log", "ios-runtime.log"):
        if (data / name).is_file():
            shutil.copy2(data / name, out / name)
    if (data / "render-replay").is_dir():
        shutil.copytree(data / "render-replay", out, dirs_exist_ok=True)
    log = out / "ios-runtime.log"
    status = mac_run.game_exit(log.read_text(errors="replace")) if finished and log.is_file() else None
    return read_result(out), seconds, status


def frame_lines(folder):
    path = Path(folder) / "frames.txt"
    return path.read_text().splitlines() if path.is_file() else []


def compare_frames(expected, got, channel_tolerance=0, fraction=0.0):
    """problems (strings) between two replay folders: frames.txt line by line, and where a hash differs and both
    kept the frame's picture, how many pixels differ"""
    problems = []
    a, b = frame_lines(expected), frame_lines(got)
    if not a:
        return [f"no frames.txt in {expected}"]
    if len(a) != len(b):
        problems.append(f"{len(a)} frames expected, {len(b)} drawn")
    differing = [(x, y) for x, y in zip(a, b) if x != y]
    for line_a, line_b in differing:
        name = line_a.split()[0]
        if line_b.split()[0] != name or line_a.split()[1] != line_b.split()[1]:
            problems.append(f"{name}: expected {line_a.split()[1]}, drew {' '.join(line_b.split()[:2])}")
            continue
        picture_a, picture_b = Path(expected) / f"{name}.bmp", Path(got) / f"{name}.bmp"
        if picture_a.is_file() and picture_b.is_file():
            data_a, data_b = picture_a.read_bytes(), picture_b.read_bytes()
            pixels, largest, where = mac_run.bmp_difference(data_a, data_b, channel_tolerance)
            if pixels > fraction * mac_run.bmp_pixels(data_a):
                problems.append(f"{name}: {pixels} pixels differ by more than {channel_tolerance}, at most {largest} "
                                f"(at {where})")
        elif channel_tolerance == 0 and fraction == 0.0:
            problems.append(f"{name}: differs (hash {line_a.split()[2]} expected, {line_b.split()[2]} drawn)")
    return problems


OPERATIONS = ("", "end", "texture create", "texture upload", "copy level", "mipmaps", "texture destroy", "buffer create",
              "buffer write", "stream reserve", "stream", "shader create", "constants", "uniforms", "clear", "draw",
              "visibility begin", "visibility end", "visibility result", "flush", "present", "stereo present")


def record_counts(path):
    """{operation name: (records, payload bytes)} of a capture"""
    counts = {}
    with gzip.open(path, "rb") as file:
        file.read(HEADER.size)
        while True:
            head = file.read(8)
            if len(head) < 8:
                break
            operation, size = struct.unpack("<II", head)
            file.seek(size, 1)
            name = OPERATIONS[operation] if operation < len(OPERATIONS) else f"operation {operation}"
            records, total = counts.get(name, (0, 0))
            counts[name] = (records + 1, total + size)
    return counts


def command_info(args):
    hashes = build_hashes()
    for path in args.captures:
        try:
            header = read_header(path)
        except (OSError, ValueError) as error:
            print(f"{path}: {error}")
            continue
        stale = staleness(header, hashes)
        print(f"{path}: {header['backend']}, frames {header['first_frame']} to "
              f"{header['first_frame'] + header['frame_count'] - 1}, label \"{header['label']}\", "
              f"{Path(path).stat().st_size / 1048576:.1f} MB, gpu.h {header['interface_hash']:08x}, front end "
              f"{header['front_end_hash']:08x}" + (f"; STALE ({stale} changed)" if stale else "; current"))
        if args.records:
            for name, (records, total) in sorted(record_counts(path).items(), key=lambda item: -item[1][1]):
                print(f"    {records:8d} {name}: {total / 1048576:.2f} MB uncompressed")


def command_replay(args):
    result, seconds, status = replay(args.app, args.capture, args.out, validation=args.validation, shots=args.shots,
                                     force=args.force, limit=args.time_limit)
    print(f"{args.capture}: exit {status}, {result.get('frames', 0)} frames, {result.get('draws', 0)} draws, "
          f"{result.get('validation_failures', '?')} validation failures, {seconds:.1f} s; results in {args.out}")
    if status != 0:
        sys.exit(f"the replay failed: {result.get('error') or 'the app did not exit cleanly'} (logs in {args.out})")


def command_check(args):
    captures = sorted(Path(args.set).glob("*.capture"))
    if not captures:
        sys.exit(f"no captures in {args.set}")
    hashes = build_hashes()
    work = Path(args.work or Path(args.set) / "last-check")
    failed, stale_front_end, total = [], [], 0.0
    started = time.monotonic()
    for capture in captures:
        name = capture.stem
        expected = Path(args.set) / "expected" / name
        try:
            stale = staleness(read_header(capture), hashes)
        except (OSError, ValueError) as error:
            failed.append(f"{name}: {error}")
            continue
        if stale == "interface":
            failed.append(f"{name}: STALE: gpu.h has changed since it was captured; record the set again")
            continue
        if stale:
            stale_front_end.append(name)
        result, seconds, status = replay(args.app, capture, work / name, shots="last", limit=args.time_limit)
        total += seconds
        problems = []
        if status != 0:
            problems.append(f"the replay failed (exit {status}): {result.get('error') or 'the app did not exit cleanly'}")
        elif result.get("validation_failures", 0):
            problems.append(f"{result['validation_failures']} validation failures (see {work / name / 'stderr.log'})")
        if status == 0 and args.record:
            shutil.rmtree(expected, ignore_errors=True)
            expected.mkdir(parents=True)
            for file in sorted((work / name).glob("frame*")):
                shutil.copy2(file, expected / file.name)
        elif status == 0:
            problems += compare_frames(expected, work / name, args.channel_tolerance, args.fraction)
        print(f"{'FAIL' if problems else 'recorded' if args.record else 'ok'}  {name}: {result.get('frames', 0)} frames, "
              f"{result.get('draws', 0)} draws, {seconds:.1f} s" + "".join(f"\n      {problem}" for problem in problems))
        failed += [f"{name}: {problem}" for problem in problems]
    if stale_front_end:
        print(f"note: the front end has changed since {len(stale_front_end)} of {len(captures)} captures were made "
              f"({', '.join(stale_front_end[:4])}{', ...' if len(stale_front_end) > 4 else ''}): they test the backend "
              f"against the old front end's calls. Record the set again to cover the front end's change.")
    print(f"render replay: {len(captures)} captures in {time.monotonic() - started:.1f} s ({total:.1f} s replaying); "
          f"{len(failed)} problems")
    if failed:
        sys.exit(1)


def command_fuzz(args):
    crashes = holes = mutations = 0
    for seed in range(args.first_seed, args.first_seed + args.seeds):
        out = Path(args.out) / f"seed-{seed}"
        result, seconds, status = replay(args.app, args.capture, out, validation="reject", fuzz=f"{seed}:{args.every}",
                                         shots="none", limit=args.time_limit)
        log = out / "fuzz.log"
        lines = log.read_text(errors="replace").splitlines() if log.is_file() else []
        mutations += result.get("fuzz_mutations", 0)
        if status is None or status not in (0, 2):
            crashes += 1
            last = next((line for line in reversed(lines) if line.startswith("mutation")), "no mutation logged")
            print(f"CRASH  seed {seed}: the app died (exit {status}) after: {last}")
        elif status == 2 or result.get("fuzz_not_refused", 0):
            holes += result.get("fuzz_not_refused", 1)
            print(f"HOLE   seed {seed}: {result.get('fuzz_not_refused')} wrong calls reached the backend:")
            for index, line in enumerate(lines):
                if line.strip() == "NOT REFUSED" and index:
                    print(f"       {lines[index - 1]}")
        else:
            print(f"ok     seed {seed}: {result.get('fuzz_mutations', 0)} wrong calls, all refused, {seconds:.1f} s")
    print(f"render replay fuzz: {args.seeds} seeds, {mutations} wrong calls, {holes} reached the backend, {crashes} crashes")
    if crashes or holes:
        sys.exit(1)


def command_across(args):
    problems = compare_frames(args.a, args.b, args.channel_tolerance, args.fraction)
    for problem in problems:
        print(problem)
    print(f"{len(frame_lines(args.a))} frames compared, {len(problems)} problems")
    if problems:
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    def with_app(command):
        command.add_argument("--app", type=Path, default=mac_run.native_app_default(),
                             help="the Mac app, built with --render-validation on (default: this checkout's)")
        command.add_argument("--time-limit", type=float, default=180.0, help="seconds before a replay is killed")

    info = commands.add_parser("info")
    info.add_argument("captures", nargs="+", type=Path)
    info.add_argument("--records", action="store_true", help="also each kind of record's count and size")
    info.set_defaults(function=command_info)
    one = commands.add_parser("replay")
    one.add_argument("capture", type=Path)
    one.add_argument("--out", type=Path, required=True)
    one.add_argument("--validation", choices=("off", "log", "strict", "reject"), default="log")
    one.add_argument("--shots", choices=("all", "last", "none"), default="all")
    one.add_argument("--force", action="store_true", help="replay a capture made against another gpu.h")
    with_app(one)
    one.set_defaults(function=command_replay)
    check = commands.add_parser("check")
    check.add_argument("set", type=Path)
    check.add_argument("--record", action="store_true", help="write the set's expected frames instead of comparing")
    check.add_argument("--work", type=Path, help="where the replays' results go (default SET/last-check)")
    check.add_argument("--channel-tolerance", type=int, default=0)
    check.add_argument("--fraction", type=float, default=0.0)
    with_app(check)
    check.set_defaults(function=command_check)
    fuzz = commands.add_parser("fuzz")
    fuzz.add_argument("capture", type=Path)
    fuzz.add_argument("--out", type=Path, required=True)
    fuzz.add_argument("--seeds", type=int, default=8)
    fuzz.add_argument("--first-seed", type=int, default=1)
    fuzz.add_argument("--every", type=int, default=8, help="one record in this many is replaced by a wrong call")
    with_app(fuzz)
    fuzz.set_defaults(function=command_fuzz)
    across = commands.add_parser("across")
    across.add_argument("a", type=Path)
    across.add_argument("b", type=Path)
    across.add_argument("--channel-tolerance", type=int, default=1)
    across.add_argument("--fraction", type=float, default=0.0001)
    across.set_defaults(function=command_across)
    args = parser.parse_args()
    if platform_not_mac():
        sys.exit("render_replay.py runs on a Mac")
    args.function(args)


def platform_not_mac():
    return sys.platform != "darwin"


if __name__ == "__main__":
    main()
