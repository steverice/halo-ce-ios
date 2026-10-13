#!/usr/bin/env python3
"""Propose a device profile from device benchmark reports.

    profile_derive.py derive REPORT... [--out PROPOSAL.toml] [--table RUNS.md]
    profile_derive.py from-runs --model AppleTV5,3 --out REPORT.json RUN_FOLDER...

`derive` reads reports the app's benchmark wrote (report.json, or the .json.gz the device's page serves) and
prints a proposal in the device-profiles format (profiles/TEMPLATE.toml), each value with the measurement
behind it. It never writes into profiles/ and never commits: a person reviews the proposal.

The targets: 60 frames a second or better in play and in the menu, 30 in cutscenes. A run holds a target when
its 99th percentile frame interval is within the target's frame time plus 5%, its CPU and GPU times at the 95th
percentile are under 85% of it, and at least 300 MB stayed free before the system's memory limit. A gameplay
profile that only holds 30 is never proposed: when nothing holds 60 the proposal says so and is marked
"below target". On a display that refreshes at 100 Hz or more, the proposal still aims for 60 and adds a line on
whether 120 is within reach.

`from-runs` builds a report from result folders of one-launch runs (step-tools/device-run.py with
debug.gpu_stats), whose logs carry a summary every 600 frames and no per-frame times, so that measurements made
before the benchmark existed can be read the same way.
"""

import argparse
import base64
import gzip
import json
import re
import struct
import sys
from pathlib import Path

PLAY_TARGET = 60.0
CUTSCENE_TARGET = 30.0
FAST_REFRESH = 100.0
MEMORY_FLOOR = 300
# the lowest settings worth shipping: below these the picture is not one we would put a name to
LOWEST = {"render_lines": 540, "anisotropic_filtering": 2, "shadow_map_size": 128, "model_lod": "auto"}
THERMAL = {0: "nominal", 1: "fair", 2: "serious", 3: "critical"}


def load_report(path):
    """a report as a dictionary, from .json or .json.gz"""
    data = Path(path).read_bytes()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    report = json.loads(data)
    if report.get("format") != 1:
        raise ValueError(f"{path}: report format {report.get('format')!r}; this script reads format 1")
    return report


def frame_times(run, name):
    """a run's per-frame times in milliseconds (frame_interval, frame_cpu or frame_gpu), or [] if it has none"""
    text = run.get(name)
    if not text:
        return []
    data = base64.b64decode(text)
    return [value / 10.0 for value in struct.unpack(f"<{len(data) // 2}H", data)]


def holds(run, frames_a_second):
    """whether a run holds a target, and the reason it does not"""
    if run.get("status") != "ok":
        return False, f"ended {run.get('status')}"
    hertz = run.get("refresh_hertz") or 0.0
    if hertz <= 0:
        return False, "no refresh rate"
    refreshes = max(1, round(hertz / frames_a_second))
    period = 1000.0 * refreshes / hertz
    if run["interval_ms"]["p99"] > period * 1.05:
        return False, f"99th percentile frame {run['interval_ms']['p99']:.1f} ms over {period * 1.05:.1f}"
    for part in ("cpu_ms", "gpu_ms"):
        if run[part]["p95"] > period * 0.85:
            return False, f"{part[:3].upper()} 95th percentile {run[part]['p95']:.1f} ms over {period * 0.85:.1f}"
    left = run.get("memory_mb", {}).get("left_least", -1)
    if 0 <= left < MEMORY_FLOOR:
        return False, f"only {left} MB left before the memory limit"
    return True, ""


def settings_key(run):
    settings = run["settings"]
    return (settings["render_lines"], settings["anisotropic_filtering"], settings["shadow_map_size"],
            1 if settings["model_lod"] == "max" else 0)


def describe(run):
    """one run's numbers, for an evidence line"""
    if run.get("status") != "ok" or "interval_ms" not in run:
        return f"{run['scene']} ended {run.get('status')}"
    return (f"{run['scene']} {run['frames_a_second']:.0f} fps, {100 * run['on_time_full']:.0f}% of frames on time, "
            f"frame 99th {run['interval_ms']['p99']:.1f} ms, CPU {run['cpu_ms']['p95']:.1f} and GPU {run['gpu_ms']['p95']:.1f} ms "
            f"at the 95th percentile")


def usable_runs(reports):
    """every run of every report, tagged with its report's code; runs that say nothing about speed are left out"""
    runs = []
    for report in reports:
        for run in report["runs"]:
            if run.get("status") in ("skipped",):
                continue
            runs.append(dict(run, report=report["code"]))
    return runs


def choose_play(runs):
    """the gameplay settings: (settings key, status, evidence runs, notes)"""
    play = [run for run in runs if run["kind"] == "gameplay"]
    notes = []
    if not play:
        return None, "no data", [], ["No gameplay run produced frames."]
    by_key = {}
    for run in play:
        by_key.setdefault(settings_key(run), []).append(run)
    scenes = {run["scene"] for run in play if run["stage"] in "AE"} or {run["scene"] for run in play}
    passing = {key: group for key, group in by_key.items() if all(holds(run, PLAY_TARGET)[0] for run in group)}
    whole = {key: group for key, group in passing.items() if {run["scene"] for run in group} >= scenes}
    if whole:
        key = max(whole)
        return key, "provisional", whole[key], notes
    if passing:
        # no single combination was run on every scene: put the answer together from the runs that held
        lines = max(key[0] for key in passing)
        at_size = {key: group for key, group in passing.items() if key[0] == lines}
        best = [lines, max(key[1] for key in at_size), max(key[2] for key in at_size), max(key[3] for key in at_size)]
        evidence = [run for group in at_size.values() for run in group]
        notes.append("Put together from runs that each changed one setting; the combination was not run on every scene. "
                     "Run the full suite again for a confirmation pass before trusting it.")
        return tuple(best), "provisional", evidence, notes
    # nothing holds 60: say so, and name the cheapest settings tried, never a 30 target
    key = min(by_key)
    worst = max(by_key[key], key=lambda run: run.get("interval_ms", {}).get("p99", 0.0))
    notes.append(f"NO SETTINGS TRIED HOLD {PLAY_TARGET:.0f} FRAMES A SECOND IN PLAY ON THIS DEVICE. The cheapest tried "
                 f"({key[0]} lines, {key[1]}x filtering, {key[2]} shadows, model detail {'max' if key[3] else 'auto'}) "
                 f"gives: {describe(worst)}.")
    floor = (LOWEST["render_lines"], LOWEST["anisotropic_filtering"], LOWEST["shadow_map_size"], 0)
    if any(a > b for a, b in zip(key, floor)):
        notes.append("Settings below these were not tried; the lowest sensible ones are "
                     f"{floor[0]} lines, {floor[1]}x filtering, {floor[2]} shadows, the game's own model detail.")
    else:
        notes.append("These are the lowest sensible settings. This device needs a decision by a person, not a profile value.")
    return key, "below target", by_key[key], notes


def choose_scene(runs, kind, target):
    """the highest render size at which every run of a scene kind holds its target: (lines, runs) or (None, [])"""
    by_lines = {}
    for run in runs:
        if run["kind"] == kind:
            by_lines.setdefault(run["settings"]["render_lines"], []).append(run)
    good = [lines for lines, group in by_lines.items() if all(holds(run, target)[0] for run in group)]
    if not good:
        return None, [run for group in by_lines.values() for run in group]
    return max(good), by_lines[max(good)]


def toml_string(text):
    return json.dumps(text, ensure_ascii=False)


def derive(reports):
    """(proposal TOML text, notes) for reports of one model"""
    models = {report["device"]["model"] for report in reports}
    if len(models) != 1:
        raise ValueError(f"reports for more than one model: {', '.join(sorted(models))}; derive one model at a time")
    device = reports[0]["device"]
    drawable = {report["profile"]["drawable_lines"] for report in reports}
    notes = []
    if len(drawable) != 1:
        notes.append(f"The reports were made at different output sizes ({sorted(drawable)} lines); the proposal uses the largest.")
    drawable = max(drawable)
    flags = sorted({flag for report in reports for flag in report.get("flags", [])})
    if flags:
        notes.append("Flags on these reports: " + ", ".join(flags) + ". Runs that ended thermal, interrupted, diverged, "
                     "crashed or killed are not used as evidence that a setting holds.")
    runs = usable_runs(reports)
    key, status, evidence, play_notes = choose_play(runs)
    notes += play_notes
    codes = ", ".join(sorted({report["code"] for report in reports}))
    source = f"device benchmark report {codes}"
    lines = [f"# Proposed by tools/profile_derive.py from {source}. Review before copying into profiles/.", "",
             "schema = 1", f"name = {toml_string(device['model'])}", 'kind = "model"',
             f"status = {toml_string(status)}", "", "[match]", f"platform = {toml_string(device['platform'])}",
             f"models = [{toml_string(device['model'].replace(' (simulator)', ''))}]", ""]
    evidence_tables = []

    def evidence_for(name, why):
        evidence_tables.extend([f"[evidence.{toml_string(name)}]", 'basis = "measured"', f"why = {toml_string(why)}",
                                f"source = {toml_string(source)}", ""])

    menu_lines, menu_runs = choose_scene(runs, "menu", PLAY_TARGET)
    cutscene_lines, cutscene_runs = choose_scene(runs, "cutscene", CUTSCENE_TARGET)
    if key is None:
        return "\n".join(lines + ["# No gameplay data: nothing to propose.", ""]), notes
    play_lines, anisotropy, shadow, lod = key
    height = max(value for value in (play_lines, menu_lines, cutscene_lines) if value)
    why = "; ".join(describe(run) for run in sorted(evidence, key=lambda run: run["scene"])[:6])
    if status == "below target":
        why = f"BELOW TARGET: no settings tried hold {PLAY_TARGET:.0f} fps. " + why
    lines += ["[display]", f"render_height = {height}", f"render_scale = {play_lines / drawable:.3f}".rstrip("0").rstrip("."),
              f"anisotropic_filtering = {anisotropy}", f"shadow_map_size = {shadow}",
              f"model_lod = {toml_string('max' if lod else 'auto')}", ""]
    evidence_for("display.render_height", f"The largest render size any scene class uses here; the display has {drawable} lines.")
    evidence_for("display.render_scale", f"{play_lines} lines in play. " + why)
    for name, value, index in (("display.anisotropic_filtering", anisotropy, 1), ("display.shadow_map_size", shadow, 2),
                               ("display.model_lod", "max" if lod else "auto", 3)):
        tried = sorted({settings_key(run)[index] for run in runs if run["kind"] == "gameplay"})
        shown = ["max" if item else "auto" for item in tried] if index == 3 else tried
        evidence_for(name, (f"{value} is the best value tried that held {PLAY_TARGET:.0f} fps in play" if status != "below target"
                            else f"{value}: the cheapest value tried; the target was not held")
                     + f" (tried: {', '.join(map(str, shown))}).")
    for scene, scene_lines, scene_runs, target in (("menu", menu_lines, menu_runs, PLAY_TARGET),
                                                   ("cutscene", cutscene_lines, cutscene_runs, CUTSCENE_TARGET)):
        if scene_lines is None:
            if scene_runs:
                notes.append(f"No {scene} run held {target:.0f} fps: " + "; ".join(describe(run) for run in scene_runs[:3])
                             + f". The {scene} keeps gameplay's size.")
            continue
        if scene_lines == play_lines:
            continue
        lines += [f"[scene.{scene}.display]", f"render_scale = {scene_lines / drawable:.3f}".rstrip("0").rstrip("."), ""]
        evidence_for(f"scene.{scene}.display.render_scale", f"{scene_lines} lines holds {target:.0f} fps: "
                     + "; ".join(describe(run) for run in scene_runs[:3]))
    hertz = max((run.get("refresh_hertz") or 0.0) for run in runs)
    if hertz >= FAST_REFRESH and status != "below target":
        at_choice = [run for run in runs if run["kind"] == "gameplay" and settings_key(run) == key]
        if at_choice and all(holds(run, hertz)[0] for run in at_choice):
            notes.append(f"120: the chosen settings also hold {hertz:.0f} fps on this display, so {hertz:.0f} can be the default.")
        else:
            lower = [run for run in runs if run["stage"] == "F"]
            reach = "; one size lower: " + "; ".join(f"{describe(run)} ({'holds' if holds(run, hertz)[0] else 'does not hold'} "
                                                     f"{hertz:.0f})" for run in lower) if lower else ""
            notes.append(f"120: the chosen settings hold {PLAY_TARGET:.0f} but not {hertz:.0f} fps on this display{reach}. "
                         f"{hertz:.0f} is an option, not the default.")
    return "\n".join(lines + evidence_tables), notes


def run_table(reports):
    """every run as a Markdown table, for the reviewer"""
    rows = ["| Report | Run | Stage | Scene | Lines | Filtering | Shadows | Detail | Status | fps | On time | Frame p99 | CPU p95 | "
            "GPU p95 | Hz | Memory left | Heat | Holds |", "|" + "---|" * 18]
    for report in reports:
        for run in report["runs"]:
            settings = run["settings"]
            target = CUTSCENE_TARGET if run["kind"] == "cutscene" else PLAY_TARGET
            good, why = holds(run, target) if "interval_ms" in run else (False, f"ended {run.get('status')}")
            measured = "interval_ms" in run
            rows.append("| " + " | ".join(str(cell) for cell in (
                report["code"], run["index"], run["stage"], run["scene"], settings["render_lines"],
                settings["anisotropic_filtering"], settings["shadow_map_size"], settings["model_lod"], run["status"],
                f"{run['frames_a_second']:.1f}" if measured else "", f"{100 * run['on_time_full']:.0f}%" if measured else "",
                f"{run['interval_ms']['p99']:.1f}" if measured else "", f"{run['cpu_ms']['p95']:.1f}" if measured else "",
                f"{run['gpu_ms']['p95']:.1f}" if measured else "", f"{run['refresh_hertz']:.1f}" if measured else "",
                run.get("memory_mb", {}).get("left_least", ""), THERMAL.get(run.get("thermal", {}).get("worst"), ""),
                f"{target:.0f}: yes" if good else f"{target:.0f}: no ({why})")) + " |")
    return "\n".join(rows) + "\n"


# ---------- reports from one-launch run folders

WINDOW = re.compile(r"frames shown for 1/2/3/4/5\+ refreshes at ([\d.]+) Hz: (\d+)/(\d+)/(\d+)/(\d+)/(\d+).*"
                    r"frame CPU ([\d.]+) ms, GPU ([\d.]+) ms \(medians; 95th ([\d.]+), ([\d.]+)\)")
SETTING = re.compile(r"^\s*(\w+)\s*=\s*(.+?)\s*$")


def folder_run(folder, index, default_lines):
    """one device-run.py result folder as a report's run, from its 600-frame windows"""
    folder = Path(folder)
    log = (folder / "stderr.log").read_text(errors="replace") if (folder / "stderr.log").exists() else ""
    runtime = (folder / "ios-runtime.log").read_text(errors="replace") if (folder / "ios-runtime.log").exists() else ""
    config = (folder / "config.toml").read_text(errors="replace") if (folder / "config.toml").exists() else ""
    values, section = {}, ""
    for line in config.splitlines():
        if line.startswith("["):
            section = line.strip("[] \t")
        elif (match := SETTING.match(line)) and not line.lstrip().startswith("#"):
            values[f"{section}.{match.group(1)}"] = match.group(2).strip('"')
    lines = int(values.get("display.render_height", 0) or 0)
    if not lines:
        found = re.search(r"render height (\d+)", runtime)
        lines = int(found.group(1)) if found else default_lines
    scene = folder.name.split("-")[0]
    kind = "menu" if scene == "menu" else "gameplay"
    run = {"index": index, "stage": "A", "scene": folder.name, "map": "" if scene == "menu" else scene, "kind": kind,
           "difficulty": "easy", "recording": None, "note": "from a one-launch run's log; 600-frame windows, no per-frame times",
           "settings": {"render_lines": lines, "anisotropic_filtering": int(values.get("display.anisotropic_filtering", 16)),
                        "shadow_map_size": int(values.get("display.shadow_map_size", 512)),
                        "model_lod": values.get("display.model_lod", "max")}}
    windows = [match.groups() for match in WINDOW.finditer(log)]
    # a level's first half is its opening cutscene and load; the menu's first window is its load
    windows = windows[len(windows) // 2:] if kind == "gameplay" else windows[1:] or windows
    if not windows:
        return dict(run, status="failed")
    hertz = float(windows[0][0])
    shown = [sum(int(window[1 + column]) for window in windows) for column in range(5)]
    frames = sum(shown)
    seconds = sum(count * (column + 1) for column, count in enumerate(shown)) / hertz
    # the 99th percentile interval, from how many refreshes frames stayed up
    reached, p99 = 0, 5
    for column, count in enumerate(shown):
        reached += count
        if reached >= 0.99 * frames:
            p99 = column + 1
            break
    left = [int(value) for value in re.findall(r"(\d+) MB left before", runtime)]
    foot = [int(value) for value in re.findall(r"footprint (\d+) MB", runtime)]

    def spread(median_column, high_column):
        medians = sorted(float(window[median_column]) for window in windows)
        return {"mean": sum(medians) / len(medians), "median": medians[len(medians) // 2],
                "p95": max(float(window[high_column]) for window in windows), "p99": 0.0, "worst": 0.0}

    return dict(run, status="ok", frames=frames, seconds=seconds, frames_a_second=frames / seconds, refresh_hertz=hertz,
                interval_ms={"mean": 1000.0 * seconds / frames, "median": 0.0, "p95": 0.0, "p99": 1000.0 * p99 / hertz, "worst": 0.0},
                cpu_ms=spread(6, 8), gpu_ms=spread(7, 9), shown_refreshes=shown, on_time_full=shown[0] / frames,
                on_time_half=(shown[0] + shown[1]) / frames,
                memory_mb={"footprint_first": foot[0] if foot else -1, "footprint_last": foot[-1] if foot else -1,
                           "footprint_peak": max(foot) if foot else -1, "left_least": min(left) if left else -1},
                thermal={"first": -1, "worst": -1, "last": -1, "changes": [], "change_count": 0})


def report_from_runs(folders, model, platform, drawable_lines, code):
    runs = [folder_run(folder, index, drawable_lines) for index, folder in enumerate(folders)]
    return {"format": 1, "code": code, "suite": 0, "mode": "runs", "started": "",
            "device": {"model": model, "chip": "", "platform": platform, "os": "", "os_build": "", "gpu": "", "gpu_family": 0,
                       "memory_mb": 0, "screen": {"width": 0, "height": drawable_lines, "maximum_frames_a_second": 60}},
            "app": {"version": "", "build": "", "image": ""},
            "profile": {"drawable_lines": drawable_lines, "render_lines": 0, "anisotropic_filtering": 0, "shadow_map_size": 0,
                        "model_lod": ""},
            "flags": ["from_run_logs"], "runs": runs}


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    derive_parser = commands.add_parser("derive", help="propose a profile from reports")
    derive_parser.add_argument("reports", nargs="+", type=Path)
    derive_parser.add_argument("--out", type=Path, help="write the proposal here (never inside profiles/)")
    derive_parser.add_argument("--table", type=Path, help="write every run as a Markdown table here")
    runs_parser = commands.add_parser("from-runs", help="build a report from device-run.py result folders")
    runs_parser.add_argument("folders", nargs="+", type=Path)
    runs_parser.add_argument("--model", required=True, help="the device's model identifier, such as AppleTV5,3")
    runs_parser.add_argument("--platform", default="tvos")
    runs_parser.add_argument("--drawable-lines", type=int, default=1080)
    runs_parser.add_argument("--code", default="RUNLOGS1")
    runs_parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(arguments)
    if args.command == "from-runs":
        report = report_from_runs(args.folders, args.model, args.platform, args.drawable_lines, args.code)
        args.out.write_text(json.dumps(report, indent=1) + "\n")
        print(f"{args.out}: {len(report['runs'])} runs")
        return 0
    if args.out and "profiles" in args.out.resolve().parts:
        parser.error("--out must not be inside a profiles folder: a person copies a reviewed proposal in")
    reports = [load_report(path) for path in args.reports]
    proposal, notes = derive(reports)
    if args.out:
        args.out.write_text(proposal)
        print(f"proposal: {args.out}")
    else:
        print(proposal)
    if args.table:
        args.table.write_text(run_table(reports))
        print(f"runs: {args.table}")
    for note in notes:
        print(f"note: {note}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
