#!/usr/bin/env python3
"""Record the profile the host is optimized with (tools/ios_build.py --host-pgo=use).

The host is the part Xcode builds: the app, the Metal and OpenGL backends, the
stereo presenter's shared code and SDL. This builds the Mac app instrumented
(tools/ios_build.py --mac --host-pgo=generate), plays the scenes below with it
on the native Mac runner (tools/mac_run.py), each run quitting by itself and
leaving a raw profile in build/host-pgo/raw, and merges them into
pgo/visr_host.profdata with the llvm-profdata of the Xcode that built the app.

Scenes, all with debug.fixed_timestep, nobody playing unless a route is given:

    menu          the main menu, Metal
    a10, b30, c40 the level's opening, Metal
    a10-stereo    a10 with display.stereo = "side_by_side": both eyes' passes
    a10-gl        a10 with the OpenGL ES backend, the iPhone's and iPad's default

--route LEVEL=NAME replays a recording made with debug.input_record (NAME.input
and NAME.actions in the runner's data folder) in that level's run, so the
profile also sees play: fights, the HUD, weapons.

Only functions that ran are kept in the profile: one that never ran on the
Mac (the headset's presenter, say) would otherwise be compiled as cold code.

The profile is a recording of this source, on the Mac. A host function changed
since, or compiled differently on another platform, no longer matches it and is
compiled as without a profile; record again after large changes to
port/ios/host, port/linux/src/gpu_gl.c or SDL. It needs the game's data (--maps,
or a runner data folder that already has it) and a logged-in display.

Usage: host_pgo_train.py [--maps DIR] [--bundle-id ID] [--route a10=a10-bench ...]
                         [--scenes "menu a10 ..."] [--output pgo/visr_host.profdata]
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / "build/host-pgo/raw"
RUNS = ROOT / "build/host-pgo/runs"
# name: (level or None for the menu, seconds of game time, extra settings)
SCENES = {
    "menu": (None, 30, ['display.renderer="metal"']),
    "a10": ("a10", 110, ['display.renderer="metal"']),
    "b30": ("b30", 120, ['display.renderer="metal"']),
    "c40": ("c40", 100, ['display.renderer="metal"']),
    "a10-stereo": ("a10", 60, ['display.renderer="metal"', 'display.stereo="side_by_side"']),
    "a10-gl": ("a10", 60, ['display.renderer="gl"']),
}


def executed_only(text):
    """a text profile (llvm-profdata merge --text) without the functions that never ran, and how many of
    each there were. A function in the profile with no executions is compiled as cold: for size, out of
    the way. That is wrong for code the Mac cannot reach but a device runs every frame (the headset's
    presenter, the HUD plane, the touch controls), so such functions are left out and compiled as
    without a profile."""
    kept, ran, idle = [], 0, 0
    for record in text.split("\n\n"):
        lines = record.strip("\n").split("\n")
        if "# Counter Values:" not in lines:
            if record.strip():
                kept.append(record.strip("\n"))
            continue
        start = lines.index("# Counter Values:") + 1
        count = int(lines[lines.index("# Num Counters:") + 1])
        if any(int(value) for value in lines[start:start + count]):
            kept.append(record.strip("\n"))
            ran += 1
        else:
            idle += 1
    return "\n\n".join(kept) + "\n", ran, idle


def run(*command, check=True):
    print("+", " ".join(str(word) for word in command), flush=True)
    return subprocess.run([str(word) for word in command], cwd=ROOT, check=check).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--maps", type=Path, help="a folder of extracted maps, for a new runner data folder")
    parser.add_argument("--bundle-id", help="the Mac app's bundle identifier (default: tools/ios_build.py's)")
    parser.add_argument("--route", action="append", default=[], metavar="LEVEL=NAME",
                        help="replay this recording in the level's Metal run (repeatable)")
    parser.add_argument("--scenes", default=" ".join(SCENES), help="the scenes to play (default: all)")
    parser.add_argument("--output", type=Path, default=ROOT / "pgo/visr_host.profdata")
    parser.add_argument("--skip-build", action="store_true", help="use the instrumented app already built")
    args = parser.parse_args()
    routes = dict(route.split("=", 1) for route in args.route)
    scenes = args.scenes.split()
    unknown = [name for name in scenes if name not in SCENES]
    if unknown:
        parser.error(f"no scene {', '.join(unknown)} (scenes: {', '.join(SCENES)})")

    shutil.rmtree(RAW, ignore_errors=True)
    shutil.rmtree(RUNS, ignore_errors=True)
    RAW.mkdir(parents=True)
    if not args.skip_build:
        build = [sys.executable, "tools/ios_build.py", "--mac", "--host-pgo=generate"]
        if args.bundle_id:
            build += ["--bundle-id", args.bundle_id]
        run(*build)

    failed = []
    for name in scenes:
        level, seconds, settings = SCENES[name]
        command = [sys.executable, "tools/mac_run.py", "run", "--runner", "native", "--exit-after", seconds,
                   "--time-limit", seconds + 420, "--set", "debug.fixed_timestep=true", "--out", RUNS / name]
        if args.maps:
            command += ["--maps", args.maps]
        if args.bundle_id:
            command += ["--bundle-id", args.bundle_id]
        for setting in settings:
            command += ["--set", setting]
        if level:
            command += ["--init", "game_difficulty_set normal", "--init", f"map_name {level}"]
            if level in routes and name == level:
                command += ["--set", f'debug.input_replay="{routes[level]}"', "--set", "display.direct_camera=false"]
        before = set(RAW.glob("*.profraw"))
        status = run(*command, check=False)
        written = set(RAW.glob("*.profraw")) - before
        print(f"host pgo: {name}: exit {status}, {len(written)} raw profile(s), "
              f"{sum(path.stat().st_size for path in written)} bytes", flush=True)
        if status or not written:
            failed.append(name)

    profiles = sorted(RAW.glob("*.profraw"))
    if not profiles:
        sys.exit("host pgo: the instrumented app wrote no profile")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    merged = RAW.parent / "all.proftext"
    run("xcrun", "llvm-profdata", "merge", "--text", "--output", merged, *profiles)
    text, ran, idle = executed_only(merged.read_text())
    executed = RAW.parent / "executed.proftext"
    executed.write_text(text)
    run("xcrun", "llvm-profdata", "merge", "--output", args.output, executed)
    run("xcrun", "llvm-profdata", "show", args.output)
    print(f"host pgo: {ran} functions ran and are in the profile; {idle} never ran and are left out", flush=True)
    print(f"host pgo: {args.output} ({args.output.stat().st_size} bytes) from {len(profiles)} runs"
          + (f"; scenes that failed or wrote nothing: {', '.join(failed)}" if failed else ""), flush=True)
    print("host pgo: the app in build/mac is the instrumented one; build again before using it", flush=True)


if __name__ == "__main__":
    main()
