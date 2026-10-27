#!/usr/bin/env python3
"""Trim a benchmark catalog's recordings to what its cases replay.

A case replays its recording from the level's start (determinism needs the whole lead-in) and stops at the end of its
window: the anchor event's time plus `lead` plus `window` seconds, 30 ticks a second. Nothing after that is played, so
the tail of the .actions, .seeds and .input files is dropped, keeping a margin. The catalog, the routes and every
recording a case does not end early are left byte-identical, so the catalog's canonical bytes, its case/route matching
and the app's validators see the same format.

Usage: trim_recordings.py SOURCE_FOLDER OUTPUT_FOLDER
"""
import math
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchmark_catalog as bc

TICKS_PER_SECOND = 30
MARGIN_TICKS = 20 * TICKS_PER_SECOND  # slack for pauses, skipped cinematics and a late window end
MARGIN_FRACTION = 0.10


def window_end_tick(case, route):
    """The last game tick, counted in .actions lines, that the case's replay can reach."""
    anchor_seconds = 0.0
    if case.get("anchor") is not None:
        anchor_seconds = bc.anchor_seconds(case["anchor"], route["events"])
    end = math.ceil((anchor_seconds + case["lead"] + case["window"]) * TICKS_PER_SECOND)
    return end + max(MARGIN_TICKS, math.ceil(end * MARGIN_FRACTION))


def _lines(path):
    return path.read_bytes().decode("ascii").splitlines(keepends=True)


def trim_recording(source, output, name, keep):
    """Write name's .input/.actions/.seeds to output, keeping the first `keep` action ticks; returns lines kept."""
    actions = _lines(source / f"{name}.actions")
    seeds = _lines(source / f"{name}.seeds")
    header_a, body_a = actions[0], actions[1:]
    header_s, body_s = seeds[0], seeds[1:]
    if len(body_a) != len(body_s):
        raise bc.CatalogError(f"{name}: actions and seeds differ in length")
    keep = min(keep, len(body_a))
    inputs = _lines(source / f"{name}.input")
    kept_input = [inputs[0]]
    for line in inputs[1:]:
        if float(line.split(None, 1)[0]) <= keep + TICKS_PER_SECOND:
            kept_input.append(line)
        else:
            break
    (output / f"{name}.actions").write_bytes("".join([header_a, *body_a[:keep]]).encode("ascii"))
    (output / f"{name}.seeds").write_bytes("".join([header_s, *body_s[:keep]]).encode("ascii"))
    (output / f"{name}.input").write_bytes("".join(kept_input).encode("ascii"))
    return keep


def trim_catalog(source, output):
    source, output = Path(source), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    cases = bc.load_catalog(source)
    done = {}
    for case in cases:
        recording = case.get("recording")
        if recording is None:
            continue
        route = bc._route(source, recording)
        keep = window_end_tick(case, route)
        done[recording] = max(done.get(recording, 0), keep)  # variants share a recording
    for path in source.iterdir():
        if path.suffix[1:] in {"input", "actions", "seeds"} and path.stem in done:
            continue
        shutil.copyfile(path, output / path.name)
    for recording, keep in done.items():
        trim_recording(source, output, recording, keep)
    bc.load_catalog(output)
    return done


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    done = trim_catalog(argv[1], argv[2])
    for recording, keep in sorted(done.items()):
        print(f"{recording}: {keep} ticks")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
