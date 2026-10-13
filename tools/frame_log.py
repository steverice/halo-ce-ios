#!/usr/bin/env python3
"""Summarize a frame log (debug.frame_log: frame-log.txt in the app's data folder; port/ios/host/gpu_metal.m).

    python3 tools/frame_log.py frame-log.txt [--session N] [--worst K]

Prints, for the last session in the file (or --session N, 1 the first), a row for each level and scene class: the
time spent, frames a second (all its frames over all its time), the worst 5-second window's frames a second, the
highest 95th and 99th percentile frame times of any window, late frames, the render size and frame-rate cap in
force, and the memory peak; then the K worst windows (default 8) with when they were. A window is one "frames"
line: up to 5 seconds, cut short at a change of scene class."""
import argparse
import sys
from collections import OrderedDict
from pathlib import Path


def parse(text):
    """[{"start": ..., "build": ..., "windows": [{field: value}]}] for each "session" line and its "frames" lines"""
    sessions = []
    for line in text.splitlines():
        kind, _, rest = line.partition(" ")
        fields = dict(part.split("=", 1) for part in rest.split() if "=" in part)
        if kind == "session":
            sessions.append({"start": fields.get("start", "?"), "build": fields.get("build", "?"), "windows": []})
        elif kind == "frames" and sessions:
            try:
                window = {"scene": fields["scene"], "level": fields["level"], "size": fields["size"], "cap": int(fields["cap"]),
                          "thermal": fields.get("thermal", "?"), "time": float(fields["time"]),
                          "frames": int(fields["frames"]), "fps": float(fields["fps"]), "mean": float(fields["mean_ms"]),
                          "p95": float(fields["p95_ms"]), "p99": float(fields["p99_ms"]), "late": int(fields["late"]),
                          "gaps": int(fields.get("gaps", 0)), "peak": int(fields.get("peak_mb", 0))}
            except (KeyError, ValueError):
                continue    # a line cut short when the app was ended
            sessions[-1]["windows"].append(window)
    return sessions


def summarize(windows):
    """rows by (level, scene, size, cap), in the order first seen"""
    rows = OrderedDict()
    for window in windows:
        key = (window["level"], window["scene"], window["size"], window["cap"])
        row = rows.setdefault(key, {"seconds": 0.0, "frames": 0, "late": 0, "worst": None, "p95": 0.0, "p99": 0.0,
                                    "peak": 0, "thermal": set(), "windows": 0})
        seconds = window["frames"] * window["mean"] / 1000.0
        row["seconds"] += seconds
        row["frames"] += window["frames"]
        row["late"] += window["late"]
        row["windows"] += 1
        # (a window of a few frames, cut by a scene change, says nothing about the rate)
        if window["frames"] >= 30 and (row["worst"] is None or window["fps"] < row["worst"]):
            row["worst"] = window["fps"]
        row["p95"] = max(row["p95"], window["p95"])
        row["p99"] = max(row["p99"], window["p99"])
        row["peak"] = max(row["peak"], window["peak"])
        row["thermal"].add(window["thermal"])
    return rows


def report(session, worst_count, out=sys.stdout):
    windows = session["windows"]
    print(f"session {session['start']}, build {session['build']}: {len(windows)} windows, "
          f"{sum(w['frames'] for w in windows)} frames, {sum(w['gaps'] for w in windows)} waits (loads, the background)", file=out)
    if not windows:
        print("no frames logged: the game did not reach its first 5 seconds of drawing, or the log is another session's",
              file=out)
        return
    print(f"{'level':10} {'scene':9} {'size':10} {'cap':>4} {'time':>7} {'fps':>6} {'worst':>6} {'p95 ms':>7} {'p99 ms':>7} "
          f"{'late':>6} {'peak MB':>8}  thermal", file=out)
    for (level, scene, size, cap), row in summarize(windows).items():
        fps = row["frames"] / row["seconds"] if row["seconds"] else 0.0
        worst = f"{row['worst']:6.1f}" if row["worst"] is not None else "     -"
        print(f"{level:10} {scene:9} {size:10} {cap or '-':>4} {row['seconds']:6.0f}s {fps:6.1f} {worst} {row['p95']:7.1f} "
              f"{row['p99']:7.1f} {row['late']:6d} {row['peak']:8d}  {'/'.join(sorted(row['thermal']))}", file=out)
    ranked = sorted((w for w in windows if w["frames"] >= 30), key=lambda w: w["fps"] / (w["cap"] or 60))[:worst_count]
    if ranked:
        print(f"\nthe {len(ranked)} slowest windows against their target (the cap, else 60):", file=out)
        for window in ranked:
            minutes, seconds = divmod(int(window["time"]), 60)
            print(f"  {minutes:3d}:{seconds:02d}  {window['level']:8} {window['scene']:9} {window['size']:10} "
                  f"{window['fps']:5.1f} fps of {window['cap'] or 60}, p99 {window['p99']:.1f} ms, {window['late']} late", file=out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path)
    parser.add_argument("--session", type=int, default=0, help="which session in the file, 1 the first (default: the last)")
    parser.add_argument("--worst", type=int, default=8, help="how many of the slowest windows to list")
    args = parser.parse_args()
    sessions = parse(args.log.read_text(errors="replace"))
    if not sessions:
        sys.exit(f"{args.log}: no session line; is this a frame log?")
    if args.session > len(sessions):
        sys.exit(f"{args.log}: {len(sessions)} sessions, no session {args.session}")
    report(sessions[args.session - 1], args.worst)


if __name__ == "__main__":
    main()
