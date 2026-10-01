#!/usr/bin/env python3
"""Run the iPad build on this Mac as a "Designed for iPad" app, and compare runs.

macOS launches an iPad app only if Xcode installed it. It also kills the app
that port/ios's CMake project builds ("Code Signature Invalid"), although the
same executable runs when a plain Xcode project signs it. So `run` generates a
small wrapper project around the CMake-built HaloCE executable and Info.plist,
installs and launches it through Xcode with the debugger off (memory_watch.c
write-protects pages and expects their faults, which would stop a debugger),
waits for the game to quit (debug.exit_after), and copies the logs,
screenshots and shader files out of the app's container. `compare` checks two
such result folders against each other.
"""

import argparse
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

# a debug.gpu_stats summary (d3d8_gl.c), after platform_log's prefix
STATS = re.compile(r"frame \d+: (.*)$")


def merge_config(text, settings):
    """config.toml text with each dotted key in settings set to its raw TOML value"""
    lines = text.splitlines()
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


def reset_settings(documents, screenshot_every, dump_shaders, replay):
    """the output settings of a run: requested outputs go into runner/, the rest are off"""
    runner = Path(documents) / "runner"
    return {
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
    """(width, height, BGRA rows) of a 32-bit BMP as write_screenshot (d3d8_gl.c) writes it"""
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    (offset,) = struct.unpack_from("<I", data, 10)
    width, height = struct.unpack_from("<ii", data, 18)
    (bits,) = struct.unpack_from("<H", data, 28)
    if bits != 32:
        raise ValueError(f"{bits}-bit BMP; expected 32")
    height = abs(height)
    return width, height, data[offset:offset + width * height * 4]


def bmp_difference(a, b):
    """(pixels whose color differs, largest channel difference); alpha is ignored,
    since write_screenshot forces it opaque"""
    width_a, height_a, pixels_a = read_bmp(a)
    width_b, height_b, pixels_b = read_bmp(b)
    if (width_a, height_a) != (width_b, height_b):
        return max(width_a * height_a, width_b * height_b), 255
    if pixels_a == pixels_b:
        return 0, 0
    differing = largest = 0
    row = width_a * 4
    for start in range(0, len(pixels_a), row):
        line_a, line_b = pixels_a[start:start + row], pixels_b[start:start + row]
        if line_a == line_b:
            continue
        for pixel in range(0, row, 4):
            deltas = [abs(line_a[pixel + channel] - line_b[pixel + channel]) for channel in range(3)]
            if any(deltas):
                differing += 1
                largest = max(largest, *deltas)
    return differing, largest


def stats_lines(log):
    """the debug.gpu_stats summaries in a log, without their frame numbers"""
    return [match.group(1) for match in map(STATS.search, log.splitlines()) if match]


def _files(folder, pattern):
    return {path.name: path for path in folder.glob(pattern)} if folder.is_dir() else {}


def compare(a, b, tolerance):
    """the differences between two result folders (empty: they match); a frame
    matches when no more than tolerance pixels differ"""
    a, b = Path(a), Path(b)
    problems = []
    for folder in ("runner/shaders", "runner/replay/replay"):
        files_a, files_b = _files(a / folder, "*.glsl"), _files(b / folder, "*.glsl")
        for name in sorted(files_a.keys() ^ files_b.keys()):
            problems.append(f"{folder}/{name} only in {a if name in files_a else b}")
        for name in sorted(files_a.keys() & files_b.keys()):
            if files_a[name].read_bytes() != files_b[name].read_bytes():
                problems.append(f"{folder}/{name} differs")
    shots_a, shots_b = _files(a / "runner/shots", "*.bmp"), _files(b / "runner/shots", "*.bmp")
    for name in sorted(shots_a.keys() ^ shots_b.keys()):
        problems.append(f"{name} only in {a if name in shots_a else b}")
    for name in sorted(shots_a.keys() & shots_b.keys()):
        differing, largest = bmp_difference(shots_a[name].read_bytes(), shots_b[name].read_bytes())
        if differing > tolerance:
            problems.append(f"{name}: {differing} pixels differ, by up to {largest}")
    logs = [folder / "stderr.log" for folder in (a, b)]
    missing = [str(log) for log in logs if not log.is_file()]
    if missing:
        problems += [f"{log} missing" for log in missing]
    else:
        stats_a, stats_b = (stats_lines(log.read_text(errors="replace")) for log in logs)
        if stats_a != stats_b:
            problems.append(f"gpu_stats differ:\n  {a}: {stats_a}\n  {b}: {stats_b}")
    return problems


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "build/mac-runner"
TARGET = "HaloRunner"
CONTAINERS = Path.home() / "Library/Containers"
DESTINATION = "platform=macOS,arch=arm64,variant=Designed for iPad"

PROJECT = """name: {target}
options:
  bundleIdPrefix: org.haloce
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
      - name: Use the CMake-built HaloCE
        basedOnDependencyAnalysis: false
        script: |
          cp "{app}/HaloCE" "$TARGET_BUILD_DIR/$EXECUTABLE_PATH"
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
"""

LAUNCH = """tell application "{xcode}"
	set doc to first workspace document whose path contains "mac-runner/{target}.xcodeproj"
	repeat 120 times
		if loaded of doc then exit repeat
		delay 1
	end repeat
	set active run destination of doc to (first run destination of doc whose name is "My Mac (Designed for iPad)")
	run doc
end tell
"""


def run_command(*args, **options):
    print("+", " ".join(str(arg) for arg in args), flush=True)
    return subprocess.run([str(arg) for arg in args], check=True, **options)


def xcode_app():
    developer = subprocess.check_output(["xcode-select", "--print-path"], text=True).strip()
    return str(Path(developer).parents[1])


def running():
    return subprocess.run(["pgrep", "-x", TARGET], capture_output=True).returncode == 0


def wait_for(condition, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(2)
    return False


def build_wrapper(args):
    RUNNER.mkdir(parents=True, exist_ok=True)
    (RUNNER / "stub.c").write_text("int main(void) { return 0; }\n")
    (RUNNER / "project.yml").write_text(PROJECT.format(
        target=TARGET, team=args.team, bundle_id=args.bundle_id, app=args.app.resolve()))
    run_command("xcodegen", "generate", "--spec", RUNNER / "project.yml", "--project", RUNNER, "--quiet")
    run_command("xcodebuild", "-project", RUNNER / f"{TARGET}.xcodeproj", "-scheme", TARGET,
                "-destination", DESTINATION, "-allowProvisioningUpdates", "build",
                stdout=subprocess.DEVNULL)


def launch():
    """install and start the wrapper through Xcode (the only way macOS accepts)"""
    xcode = xcode_app()
    run_command("open", "-a", xcode, RUNNER / f"{TARGET}.xcodeproj")
    run_command("osascript", input=LAUNCH.format(xcode=xcode, target=TARGET), text=True)
    if not wait_for(running, 300):
        sys.exit(f"{TARGET} did not start within 300 seconds; see Xcode's report navigator")


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


def prepare(args, documents):
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
    if args.replay:
        shutil.copytree(args.replay, runner / "replay",
                        ignore=lambda folder, names: [n for n in names if not n.endswith((".vsh", ".key"))])
    settings = reset_settings(documents, args.screenshot_every, args.dump_shaders, bool(args.replay))
    settings["debug.exit_after"] = f"{float(args.exit_after)}"
    for assignment in args.set:
        key, value = assignment.split("=", 1)
        settings[key.strip()] = value.strip()
    config = documents / "config.toml"
    config.write_text(merge_config(config.read_text() if config.is_file() else "", settings))
    init = documents / "init.txt"
    if args.init:
        init.write_text("\n".join(args.init) + "\n")
    elif init.exists():
        init.unlink()
    for name in ("stderr.log", "debug.txt"):
        (documents / name).unlink(missing_ok=True)
    (documents / "stderr.log").touch()


def collect(documents, out):
    out.mkdir(parents=True, exist_ok=True)
    for name in ("ios-runtime.log", "stderr.log", "debug.txt", "config.toml"):
        if (documents / name).is_file():
            shutil.copy2(documents / name, out / name)
    if (documents / "runner").is_dir():
        shutil.copytree(documents / "runner", out / "runner", dirs_exist_ok=True)


def run(args):
    if not (args.app / "HaloCE").is_file():
        sys.exit(f"no CMake-built app at {args.app}; run tools/ios_build.py --team ... first")
    build_wrapper(args)
    documents = container_documents(args)
    prepare(args, documents)
    launch()
    finished = wait_for(lambda: not running(), args.exit_after + 120)
    if not finished:
        subprocess.run(["pkill", "-x", TARGET])
    collect(documents, args.out)
    if not finished:
        sys.exit(f"{TARGET} was still running {args.exit_after + 120} seconds after launch and was killed; "
                 f"logs are in {args.out}")
    print(f"results: {args.out}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    run_parser = commands.add_parser("run", help="run the game once and collect its results")
    run_parser.add_argument("--team", required=True, help="Apple development team ID")
    run_parser.add_argument("--bundle-id", default="org.haloce.macrunner")
    run_parser.add_argument("--app", type=Path, default=ROOT / "build/ios/app-device/Release-iphoneos/HaloCE.app",
                            help="the CMake-built device app (tools/ios_build.py --team ...)")
    run_parser.add_argument("--out", type=Path, required=True, help="folder to copy the results to")
    run_parser.add_argument("--xiso", type=Path, help="the player's XISO, imported on the first run")
    run_parser.add_argument("--exit-after", type=float, default=60.0, help="seconds before the game quits")
    run_parser.add_argument("--set", action="append", default=[], metavar="SECTION.KEY=VALUE",
                            help="a config.toml setting, value in TOML syntax (repeatable)")
    run_parser.add_argument("--init", action="append", default=[], metavar="COMMAND",
                            help="a console command for init.txt, e.g. 'map_name a10' (repeatable)")
    run_parser.add_argument("--screenshot-every", type=int, default=0, metavar="FRAMES")
    run_parser.add_argument("--dump-shaders", action="store_true")
    run_parser.add_argument("--replay", type=Path, help="a folder of recorded .vsh/.key shader inputs")
    compare_parser = commands.add_parser("compare", help="compare two result folders")
    compare_parser.add_argument("a", type=Path)
    compare_parser.add_argument("b", type=Path)
    compare_parser.add_argument("--tolerance", type=int, default=0, help="pixels a frame may differ by")
    args = parser.parse_args()
    if args.command == "run":
        run(args)
    else:
        problems = compare(args.a, args.b, args.tolerance)
        print("\n".join(problems) if problems else "match")
        sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
