#!/usr/bin/env python3
"""Compare the routes replays took, and write a recording's own route.

    benchmark_routes.py compare REPORT_OR_RUN...                     where the replays of each scene part ways
    benchmark_routes.py checks REPORT_OR_RUN... --out FOLDER         FOLDER/RECORDING.checks for each recording whose
                                                                     replays all agree

The device benchmark writes player 1's position every 300 ticks of a replay into each run's note
("tick:x,y,z;tick:x,y,z;..."). Replays of one recording that agree have stayed on its route; the first of them
becomes RECORDING.checks beside the recording (port/benchmark/recordings), which the benchmark compares against
during every later replay. Inputs are report.json files (or the .json.gz a device serves) or single run-NN.json
files.
"""

import argparse
import gzip
import json
import math
import sys
from pathlib import Path

# world units; the benchmark's own tolerance (ROUTE_TOLERANCE in port/linux/src/benchmark_suite.c)
TOLERANCE = 2.0
# two replays' points belong together when their ticks are this close (a point is taken at the first frame at or
# after each 300th tick, so the tick itself varies by a frame's worth)
TICK_WINDOW = 150


def load_runs(path):
    """(label, run) for each run of a report, or the one run of a run file"""
    data = Path(path).read_bytes()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    loaded = json.loads(data)
    runs = loaded["runs"] if "runs" in loaded else [loaded]
    return [(f"{Path(path).parent.name}/{Path(path).name}#{run['index']}", run) for run in runs]


def route_points(note):
    """[(tick, x, y, z)] from a run's note; [] when the note is not a route"""
    points = []
    for part in (note or "").split(";"):
        try:
            tick, position = part.split(":")
            x, y, z = (float(value) for value in position.split(","))
            points.append((float(tick), x, y, z))
        except ValueError:
            return []
    return points


def compare_routes(a, b):
    """(points compared, worst distance, the tick where the routes first part by more than TOLERANCE or None)"""
    compared, worst, parted = 0, 0.0, None
    for tick, x, y, z in a:
        near = min(b, key=lambda point: abs(point[0] - tick), default=None)
        if near is None or abs(near[0] - tick) > TICK_WINDOW:
            continue
        distance = math.dist((x, y, z), near[1:])
        compared += 1
        worst = max(worst, distance)
        if distance > TOLERANCE and parted is None:
            parted = tick
    return compared, worst, parted


def by_recording(paths):
    """recording name -> [(label, status, points)] for runs that replayed a recording and logged a route"""
    found = {}
    for path in paths:
        for label, run in load_runs(path):
            points = route_points(run.get("note"))
            if run.get("recording") and points:
                found.setdefault(run["recording"], []).append((label, run.get("status"), points))
    return found


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("compare", "checks"):
        sub = commands.add_parser(name)
        sub.add_argument("inputs", nargs="+", type=Path)
        if name == "checks":
            sub.add_argument("--out", type=Path, required=True, help="the recordings folder")
            sub.add_argument("--minimum", type=int, default=3, help="replays that must agree (default 3)")
    args = parser.parse_args(arguments)
    failed = False
    for recording, replays in sorted(by_recording(args.inputs).items()):
        first_label, _, first = replays[0]
        print(f"{recording}: {len(replays)} replays, {len(first)} points in the first ({first_label}), "
              f"to tick {first[-1][0]:.0f}")
        agree = True
        for label, status, points in replays[1:]:
            compared, worst, parted = compare_routes(first, points)
            short = len(points) < len(first) - 1
            verdict = (f"PARTS at tick {parted:.0f}" if parted is not None else
                       "ENDS EARLY" if short else "agrees")
            agree &= parted is None and not short
            print(f"  {label} ({status}): {compared} points compared, worst {worst:.2f} world units: {verdict}")
        if args.command == "checks":
            if not agree or len(replays) < args.minimum:
                print(f"  no {recording}.checks written: " + ("the replays do not agree" if not agree else
                      f"{len(replays)} replays, {args.minimum} needed"))
                failed = True
                continue
            target = args.out / f"{recording}.checks"
            target.write_text(";".join(f"{tick:.0f}:{x:.2f},{y:.2f},{z:.2f}" for tick, x, y, z in first) + "\n")
            print(f"  wrote {target}")
        else:
            failed |= not agree
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
