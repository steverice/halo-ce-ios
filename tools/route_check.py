#!/usr/bin/env python3
"""Replay a recorded route on the native Mac runner and check that the player follows it.

A route check names a recording and the positions player 1 must reach on it, in a text file of its own
(tools/route_checks/NAME.checks):

    recording = port/benchmark/recordings/a10-bench.input
    map = a10
    difficulty = hard
    exit_after = 110
    # segment tick x,y,z tolerance what the point shows
    1 180 -56.41,0.41,0.42 0.5 still in the cryo tube
    1 1080 -45.50,-13.56,0.00 2.0 out of the cryo bay

The game runs once per --runs with debug.input_replay, debug.fixed_timestep and debug.input_replay_route, which
logs "debug.input_replay: route segment S tick T at X,Y,Z (...)" every few ticks (port/linux/src/input_replay.c).
Each check passes when the logged position at its segment and tick is within its tolerance, in world units.
A segment is a stretch of the recording between two times the game time went back (a skipped cinematic, a
revert to a checkpoint), so a point is keyed the same way the replayed actions are.

    python3 tools/route_check.py [--runs N] [--out DIR] [--app APP] [--maps MAPS] tools/route_checks/a10-bench.checks
    python3 tools/route_check.py --suite tools/route_checks/a10-bench.checks   # through the benchmark suite
    python3 tools/route_check.py --log stderr.log tools/route_checks/a10-bench.checks   # check a run already made

Exits 0 when every run passes every check, 1 when one does not, 2 when a run could not be made.
"""

import argparse
import math
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# how often the route is logged, in ticks; every check's tick must be a multiple of it
ROUTE_TICKS = 30
ROUTE_LINE = re.compile(
    r"debug\.input_replay: route segment (-?\d+) tick (\d+) at (?:(none)|(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)) \((.*)\)")


@dataclass
class Check:
    segment: int
    tick: int
    position: tuple
    tolerance: float
    what: str


@dataclass
class RouteCheck:
    recording: Path
    map: str
    difficulty: str
    exit_after: float
    checks: list
    suite: str = ""


def parse_checks(text, base=ROOT):
    """a .checks file's settings and points (see the module's docstring)"""
    settings, checks = {}, []
    for number, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" in line:
            key, value = (part.strip() for part in line.split("=", 1))
            settings[key] = value
            continue
        fields = line.split(None, 4)
        if len(fields) < 4:
            raise ValueError(f"line {number}: expected 'segment tick x,y,z tolerance [what]': {line}")
        position = tuple(float(value) for value in fields[2].split(","))
        if len(position) != 3:
            raise ValueError(f"line {number}: a position is x,y,z: {fields[2]}")
        tick = int(fields[1])
        if tick % ROUTE_TICKS:
            raise ValueError(f"line {number}: tick {tick} is not a multiple of {ROUTE_TICKS}, so it is never logged")
        checks.append(Check(int(fields[0]), tick, position, float(fields[3]), fields[4] if len(fields) > 4 else ""))
    for key in ("recording", "map", "difficulty"):
        if key not in settings:
            raise ValueError(f"no '{key} =' line")
    if not checks:
        raise ValueError("no points to check")
    return RouteCheck(base / settings["recording"], settings["map"], settings["difficulty"],
                      float(settings.get("exit_after", "120")), checks, settings.get("suite", ""))


def parse_route(text):
    """{(segment, tick): (x, y, z) or None} from a run's log; the first line for a segment and tick wins"""
    route = {}
    for match in ROUTE_LINE.finditer(text):
        key = (int(match.group(1)), int(match.group(2)))
        if key in route:
            continue
        route[key] = None if match.group(3) else tuple(float(match.group(index)) for index in (4, 5, 6))
    return route


def evaluate(route_check, route):
    """[(check, passed, why)] for each point"""
    results = []
    for check in route_check.checks:
        key = (check.segment, check.tick)
        if key not in route:
            results.append((check, False, f"segment {check.segment} tick {check.tick} was never reached"))
            continue
        position = route[key]
        if position is None:
            results.append((check, False, f"segment {check.segment} tick {check.tick}: player 1 had no body"))
            continue
        distance = math.dist(position, check.position)
        where = ",".join(f"{value:.2f}" for value in position)
        results.append((check, distance <= check.tolerance,
                        f"segment {check.segment} tick {check.tick} at {where}: {distance:.2f} from the point "
                        f"(tolerance {check.tolerance:g})"))
    return results


def report(label, results):
    failed = [result for result in results if not result[1]]
    for check, passed, why in results:
        print(f"{label}: {'ok  ' if passed else 'FAIL'} {why}{' - ' + check.what if check.what else ''}")
    print(f"{label}: {'passed' if not failed else f'{len(failed)} of {len(results)} points failed'}")
    return not failed


def run_once(route_check, out, app, maps, suite=False):
    """one replay on the native runner; its stderr.log's text, or None if the run could not be made. With suite,
    the replay is the benchmark suite's scene (the way the Apple TV and the simulators replay a route), which
    loads the map and sets its difficulty itself"""
    command = [sys.executable, str(ROOT / "tools/mac_run.py"), "run", "--runner", "native", "--out", str(out)]
    if suite:
        command += ["--benchmark-suite", f"quick:{route_check.suite}", "--time-limit", "1800"]
    else:
        command += ["--init", f"game_difficulty_set {route_check.difficulty}", "--init", f"map_name {route_check.map}",
                    "--exit-after", f"{route_check.exit_after:g}",
                    "--set", f'debug.input_replay="{route_check.recording.resolve()}"']
    command += ["--set", "debug.fixed_timestep=true", "--set", f"debug.input_replay_route={ROUTE_TICKS}",
                "--set", "display.direct_camera=false"]
    if app:
        command += ["--app", str(app)]
    if maps:
        command += ["--maps", str(maps)]
    finished = subprocess.run(command, cwd=ROOT)
    log = out / "stderr.log"
    if finished.returncode or not log.is_file():
        print(f"the run failed (mac_run.py exit {finished.returncode}); see {out}")
        return None
    return log.read_text(errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("checks", type=Path, help="a .checks file (tools/route_checks)")
    parser.add_argument("--runs", type=int, default=1, help="replays to make; each must pass")
    parser.add_argument("--out", type=Path, default=ROOT / "build/route-check", help="where each run's results go")
    parser.add_argument("--app", type=Path, help="the Mac app (default: mac_run.py's, tools/ios_build.py --mac)")
    parser.add_argument("--maps", type=Path, help="a folder of extracted maps, for a new data folder")
    parser.add_argument("--suite", action="store_true",
                        help="replay the file's `suite =` scene through the benchmark suite instead of "
                             "debug.input_replay with the map and difficulty set from the command line")
    parser.add_argument("--log", type=Path, action="append", default=[],
                        help="check this run's stderr.log instead of making a run (repeatable)")
    args = parser.parse_args()
    route_check = parse_checks(args.checks.read_text())
    passed = True
    if args.log:
        for log in args.log:
            passed &= report(str(log), evaluate(route_check, parse_route(log.read_text(errors="replace"))))
        return 0 if passed else 1
    if args.suite and not route_check.suite:
        print("the checks file has no `suite =` line")
        return 2
    if not route_check.recording.is_file():
        print(f"no recording at {route_check.recording}")
        return 2
    for run in range(1, args.runs + 1):
        out = args.out / f"{args.checks.stem}-{run}"
        text = run_once(route_check, out, args.app, args.maps, args.suite)
        if text is None:
            return 2
        passed &= report(f"{args.checks.stem} run {run}", evaluate(route_check, parse_route(text)))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
