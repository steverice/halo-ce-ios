#!/usr/bin/env python3
"""Does a recording replay alike at different frame rates? An end-to-end check on the Mac runner, with nobody
at a controller.

    replay_check.py --out FOLDER [--map b30] [--difficulty normal] [--bot 7] [--seconds 120]

1. Records a take: the game's scripted test input (debug.test_input = "bot:N") plays the map while the automatic
   recorder (debug.input_record = "auto") writes recordings/take-MAP-TIME.{input,actions,seeds,route}.
2. Replays the take through the device benchmark's suite at two frame rates (display.frame_rate), twice over:
   with the take's .seeds file (both random seeds set at every tick) and without it (only the .route's seeds).
   Each replay notes player 1's position every 300 ticks.
3. Compares the two frame rates' routes in each pair (tools/benchmark_routes.py) and prints where they part.

The expected result, if per-tick seeds are what makes a replay hold: without .seeds the two replays part, with
.seeds they do not. Exit status 0 when the with-seeds pair agrees, 1 when it parts, 2 when a step failed.

Needs tools/ios_build.py --mac first. Everything is written under FOLDER; the take is tried as a drop-in route in
the runner's data folder (benchmark/local-routes), which this removes again.
"""

import argparse
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchmark_routes  # noqa: E402
import mac_run  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
RATES = ("30", "unlimited")


def runner(out, *options):
    """one tools/mac_run.py run into out; its exit status"""
    command = [sys.executable, str(ROOT / "tools/mac_run.py"), "run", "--out", str(out), "--set", 'display.renderer="metal"',
               *options]
    print("+ " + " ".join(command), flush=True)
    return subprocess.run(command, cwd=ROOT).returncode


def newest_take(folder, map_name):
    """the base path (no extension) of the newest complete take of a map in a recordings folder, or None"""
    takes = sorted(folder.glob(f"take-{map_name}-*.actions"), key=lambda path: path.stat().st_mtime)
    for actions in reversed(takes):
        base = actions.with_suffix("")
        if all(base.with_suffix(suffix).is_file() for suffix in (".input", ".route")):
            return base
    return None


def route_text(take_route, scene):
    """the take's .route as a suite scene's: named, and measured from its first tick to its end"""
    kept = [line for line in take_route.read_text().splitlines() if not line.startswith(("scene", "lead", "window", "kind"))]
    return "\n".join([f"scene = {scene}", "kind = gameplay", "lead = 0", "window = 0", *kept]) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--map", default="b30")
    parser.add_argument("--difficulty", default="normal", choices=("easy", "normal", "hard", "impossible"))
    parser.add_argument("--bot", type=int, default=7, help="the scripted input's seed")
    parser.add_argument("--seconds", type=float, default=120.0, help="how long the bot plays")
    parser.add_argument("--take", type=Path, help="skip the recording: replay this take (its path without an extension)")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    app = mac_run.native_app_default()
    if not (app / "Contents/Info.plist").is_file():
        sys.exit(f"no native app at {app}; run tools/ios_build.py --mac first")
    with (app / "Contents/Info.plist").open("rb") as file:
        data = mac_run.native_data_folder(plistlib.load(file)["CFBundleIdentifier"])

    take = args.take
    if take is None:
        # the recording run: real time (a take is of a played session), the bot at the controller
        status = runner(args.out / "record", "--exit-after", str(args.seconds), "--set", 'debug.input_record="auto"',
                        "--set", f'debug.test_input="bot:{args.bot}"', "--init", f"game_difficulty_set {args.difficulty}",
                        "--init", f"map_name {args.map}")
        take = newest_take(args.out / "record" / "recordings", args.map)
        if take is None:
            print(f"replay_check: the recording run (exit {status}) left no take of {args.map}")
            return 2
    seeds = take.with_suffix(".seeds")
    lines = len(take.with_suffix(".actions").read_text().splitlines()) - 1
    print(f"replay_check: take {take.name}: {lines} ticks, {'with' if seeds.is_file() else 'WITHOUT'} a .seeds file", flush=True)
    if not seeds.is_file():
        print("replay_check: the recorder wrote no .seeds file, so there is nothing to compare with and without")
        return 2

    local = data / "benchmark" / "local-routes"
    results = {}
    try:
        for label, with_seeds in (("without-seeds", False), ("with-seeds", True)):
            shutil.rmtree(local, ignore_errors=True)
            local.mkdir(parents=True)
            for suffix in (".input", ".actions") + ((".seeds",) if with_seeds else ()):
                shutil.copyfile(take.with_suffix(suffix), local / f"check{suffix}")
            (local / "check.route").write_text(route_text(take.with_suffix(".route"), "check"))
            for rate in RATES:
                out = args.out / f"{label}-{rate}"
                runner(out, "--benchmark-suite", "quick:check", "--time-limit", str(int(args.seconds * 3 + 600)),
                       "--set", f'display.frame_rate="{rate}"')
                results[(label, rate)] = out / "benchmark" / "report.json"
    finally:
        shutil.rmtree(local, ignore_errors=True)

    verdicts = {}
    for label in ("without-seeds", "with-seeds"):
        reports = [results[(label, rate)] for rate in RATES]
        verdicts[label] = "failed"
        if not all(report.is_file() for report in reports):
            print(f"replay_check: {label}: a replay left no report ({', '.join(str(r) for r in reports if not r.is_file())})")
            continue
        replays = benchmark_routes.by_recording(reports).get("check", [])
        if len(replays) < 2:
            print(f"replay_check: {label}: {len(replays)} of 2 replays logged a route")
            continue
        compared, worst, parted = benchmark_routes.compare_routes(replays[0][2], replays[1][2])
        ends = [points[-1][0] for _, _, points in replays]
        statuses = [status for _, status, _ in replays]
        verdicts[label] = "agree" if parted is None else "part"
        print(f"replay_check: {label}: frame rates {' and '.join(RATES)}: {compared} points compared, worst "
              f"{worst:.2f} world units, " + (f"PART at tick {parted:.0f}" if parted is not None else "agree")
              + f"; routes end at ticks {ends[0]:.0f} and {ends[1]:.0f}; runs ended {statuses[0]} and {statuses[1]}")
    return {"agree": 0, "part": 1, "failed": 2}[verdicts["with-seeds"]]

if __name__ == "__main__":
    sys.exit(main())
