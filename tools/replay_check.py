#!/usr/bin/env python3
"""Does a recording replay alike at different frame rates? An end-to-end check on the Mac runner, with nobody
at a controller.

    replay_check.py --out FOLDER [--map b30] [--difficulty normal] [--bot 7] [--seconds 120] [--enemy-multiplier 2]

1. Records a take: the game's scripted test input (debug.test_input = "bot:N") plays the map while the automatic
   recorder (debug.input_record = "auto") writes recordings/take-MAP-TIME.{input,actions,seeds,route}. With
   --enemy-multiplier the take is recorded with debug.enemy_multiplier, which its .route carries and the catalog
   case repeats, so the replays meet the same extra enemies.
2. Replays the take, with its .seeds file (both random seeds set at every tick), through the device benchmark's
   suite at two frame rates (display.frame_rate), from an unsigned local catalog (write_local_routes; a build
   that carries routes runs it, and its reports say catalog_signed false). Each replay notes player 1's position
   every 300 ticks.
3. Compares the two frame rates' routes (tools/benchmark_routes.py) and prints where they part.

With --offscreen the second replay of each pair is the benchmark's offscreen mode (debug.benchmark_offscreen: one
tick a frame, as fast as the Mac draws) in place of an unlimited frame rate, so the check says whether that mode
replays as real time does.

Exit status 0 when the pair agrees, 1 when it parts, 2 when a step failed.

Needs tools/ios_build.py --mac first. Everything is written under FOLDER; the take is tried as a drop-in route in
the runner's data folder (benchmark/local-routes), which this removes again.
"""

import argparse
import json
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchmark_catalog  # noqa: E402
import benchmark_routes  # noqa: E402
import mac_run  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
RATES = ("30", "unlimited")
# the suite runs only a catalog's cases, and a catalog case needs its .seeds file, so a replay without the take's
# per-tick seeds is no longer run
LABELS = ("with-seeds",)


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


def route_text(take_route, scene, window):
    """the take's .route as a suite scene's: named, and measured for window seconds from its first tick"""
    kept = [line for line in take_route.read_text().splitlines() if not line.startswith(("scene", "lead", "window", "kind"))]
    return "\n".join([f"scene = {scene}", "kind = gameplay", "lead = 0", f"window = {window}", *kept]) + "\n"


def write_local_routes(take, local, scene):
    """the take as benchmark/local-routes: its files under the scene's name, a route measured over all but its last
    second, and the unsigned benchmark-catalog.json a build that carries routes runs it from (host_benchmark.m's
    local_catalog_folder; such reports say catalog_signed false). Raises benchmark_catalog.CatalogError when the
    app would refuse it."""
    local.mkdir(parents=True, exist_ok=True)
    for suffix in (".input", ".actions", ".seeds"):
        shutil.copyfile(take.with_suffix(suffix), local / f"{scene}{suffix}")
    ticks = len(take.with_suffix(".actions").read_text().splitlines()) - 1
    window = max(1, ticks // 30 - 1)
    values = dict(line.split(" = ", 1) for line in take.with_suffix(".route").read_text().splitlines() if " = " in line)
    multiplier = float(values.get("enemy_multiplier", 1))
    (local / f"{scene}.route").write_text(route_text(take.with_suffix(".route"), scene, window))
    case = {"kind": "gameplay", "order": 0, "map": values["map"], "difficulty": values["difficulty"], "recording": scene,
            "lead": 0, "window": window}
    if multiplier > 1:
        case["enemy_multiplier"] = multiplier
    catalog = {"schema": benchmark_catalog.CATALOG_SCHEMA, "cases": {scene: case}}
    (local / benchmark_catalog.CATALOG_NAME).write_bytes(benchmark_catalog._canonical(catalog))
    benchmark_catalog.load_catalog(local)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--map", default="b30")
    parser.add_argument("--difficulty", default="normal", choices=("easy", "normal", "hard", "impossible"))
    parser.add_argument("--bot", type=int, default=7, help="the scripted input's seed")
    parser.add_argument("--seconds", type=float, default=120.0, help="how long the bot plays")
    parser.add_argument("--enemy-multiplier", type=float, default=1.0,
                        help="record the take with debug.enemy_multiplier (1 off); its replays use the recorded value")
    parser.add_argument("--take", type=Path, help="skip the recording: replay this take (its path without an extension)")
    parser.add_argument("--offscreen", action="store_true",
                        help="replay the second of each pair offscreen (debug.benchmark_offscreen), not unlimited")
    args = parser.parse_args()
    rates = (RATES[0], "offscreen") if args.offscreen else RATES
    args.out.mkdir(parents=True, exist_ok=True)

    app = mac_run.native_app_default()
    if not (app / "Contents/Info.plist").is_file():
        sys.exit(f"no native app at {app}; run tools/ios_build.py --mac first")
    with (app / "Contents/Info.plist").open("rb") as file:
        data = mac_run.native_data_folder(plistlib.load(file)["CFBundleIdentifier"])

    take = args.take
    if take is None:
        # the recording run: real time (a take is of a played session), the bot at the controller
        extra = (["--set", f"debug.enemy_multiplier={args.enemy_multiplier}", "--set", "debug.input_replay_route=300"]
                 if args.enemy_multiplier != 1.0 else [])
        status = runner(args.out / "record", "--exit-after", str(args.seconds), "--set", 'debug.input_record="auto"', *extra,
                        # (the suite refuses a take with the original audio-driven mouths)
                        "--set", "debug.benchmark_lip_sync=true",
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
        for label in LABELS:
            shutil.rmtree(local, ignore_errors=True)
            write_local_routes(take, local, "check")
            for rate in rates:
                out = args.out / f"{label}-{rate}"
                mode = (["--set", "debug.benchmark_offscreen=true"] if rate == "offscreen" else
                        ["--set", f'display.frame_rate="{rate}"'])
                runner(out, "--benchmark-suite", "quick:check", "--time-limit", str(int(args.seconds * 3 + 600)), *mode)
                results[(label, rate)] = out / "benchmark" / "report.json"
    finally:
        shutil.rmtree(local, ignore_errors=True)

    verdicts = {}
    for label in LABELS:
        reports = [results[(label, rate)] for rate in rates]
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
        print(f"replay_check: {label}: frame rates {' and '.join(rates)}: {compared} points compared, worst "
              f"{worst:.2f} world units, " + (f"PART at tick {parted:.0f}" if parted is not None else "agree")
              + f"; routes end at ticks {ends[0]:.0f} and {ends[1]:.0f}; runs ended {statuses[0]} and {statuses[1]}")
    return {"agree": 0, "part": 1, "failed": 2}[verdicts["with-seeds"]]

if __name__ == "__main__":
    sys.exit(main())
