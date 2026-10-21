#!/usr/bin/env python3
"""Cost-run suites for the device cost model: bounds first, then one knob at a time where tuning can matter.

A suite is a benchmark catalog whose extra cases are variants of the workloads' own (benchmark_catalog.py:
`variant_of`, with the `settings` the run sets), run in one launch of the Mac or device runner with
`debug.benchmark_offscreen = true` and `debug.benchmark_suite = "cost:<variant ids>"`. The knobs and their
values come from cost-model/visual-ranking.toml (`order` runs best to cheapest), so a knob that lands later is a
new table there and an entry in benchmark_catalog.CASE_SETTINGS."""
import argparse
import json
import shutil
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

if __package__:
    from . import benchmark_catalog as bc
    from . import profile_cost_model as cost
else:
    import benchmark_catalog as bc
    import profile_cost_model as cost

RANKING = cost.COST / "visual-ranking.toml"
# the visual ranking's name for the render height, and the catalog setting it becomes
LINES_KNOB = cost.RESOLUTION_KEY
LINES_SETTING = "display.render_height"
FLOOR_LINES = 540
OFAT_LINES = (FLOOR_LINES, 720, 0)
FLOOR_FPS = 30
TARGET_FPS = 60
CLASSES = ("handheld", "tv")
BOOLEAN_TEXT = {"on": True, "true": True, "off": False, "false": False}
# a workload the suite measures whatever the data says: these ids are looked up in the catalog, never built in
MULTIPLAYER_DEFAULTS = ("mp16-chillout", "mp32-bloodgulch", "mp-sidewinder-128")
CASE_FILES = (".input", ".actions", ".seeds", ".route", ".checks")


@dataclass
class SuiteRun:
    id: str
    workload: str
    settings: dict


@dataclass
class CatalogSuite:
    """Workloads (catalog cases, by id) and the runs made of them. Each run is a variant case of its workload."""
    workloads: dict
    runs: list = field(default_factory=list)
    not_recommended: list = field(default_factory=list)
    no_tuning: list = field(default_factory=list)

    def catalog(self):
        """The suite's catalog document: its workloads as they are, then each run as a variant of one."""
        cases, order = {}, 0
        for run in self.runs:
            if run.workload in cases:
                continue
            case = {key: value for key, value in self.workloads[run.workload].items() if key not in ("id", "variant_of")}
            case["order"] = order
            order += 1
            cases[run.workload] = case
        for run in self.runs:
            cases[run.id] = {"variant_of": run.workload, "order": order, "settings": dict(run.settings)}
            order += 1
        return {"schema": bc.CATALOG_SCHEMA, "cases": cases}

    def selection(self):
        """The mode the app is launched with: only the runs, not the workloads they vary."""
        return "cost:" + ",".join(run.id for run in self.runs)

    def chunks(self, limit=bc.MAXIMUM_CASES):
        """Suites of at most `limit` catalog cases (a workload counts once per suite it appears in)."""
        result = []
        current = []
        for run in self.runs:
            used = {item.workload for item in current} | {run.workload}
            if len(current) + 1 + len(used) > limit:
                result.append(current)
                current = []
            current.append(run)
        if current:
            result.append(current)
        return [CatalogSuite(self.workloads, runs) for runs in result]

    def write(self, folder, recordings):
        """The catalog and the recordings its workloads replay, in `folder` (new or empty), ready to run."""
        folder = Path(folder)
        folder.mkdir(parents=True, exist_ok=True)
        document = self.catalog()
        for case_id, case in document["cases"].items():
            name = case.get("recording")
            if name:
                for suffix in CASE_FILES:
                    source = Path(recordings) / f"{name}{suffix}"
                    if source.exists():
                        shutil.copyfile(source, folder / source.name)
        (folder / bc.CATALOG_NAME).write_bytes(bc._canonical(document))
        bc.load_catalog(folder)
        return self.selection()


def read_ladder(klass="handheld", path=RANKING):
    """The knobs of an output class, best value first: {config name: [value text, ...]}."""
    with open(path, "rb") as handle:
        table = tomllib.load(handle)[klass]
    return {knob: [str(value) for value in entry["order"]] for knob, entry in table.items()}


def setting_name(knob):
    return LINES_SETTING if knob == LINES_KNOB else knob


def setting_value(knob, text):
    """A ladder value as the catalog's `settings` carries it."""
    name = setting_name(knob)
    spec = bc.CASE_SETTINGS.get(name)
    if spec is None:
        raise ValueError(f"{knob} is not a setting a catalog case can carry")
    kind = spec[0]
    if kind == "boolean":
        return BOOLEAN_TEXT[text]
    if kind == "integer":
        return int(text)
    if kind == "number":
        return float(text)
    return text


def _settings(values):
    return {setting_name(knob): setting_value(knob, text) for knob, text in values.items()}


def _bound(ladder, lowest):
    values = {}
    for knob, order in ladder.items():
        if knob == LINES_KNOB:
            values[knob] = str(FLOOR_LINES) if lowest else "0"
        else:
            values[knob] = order[-1] if lowest else order[0]
    return _settings(values)


def _run_id(workload, tag):
    name = f"{workload}-{tag}"
    if len(name) > 47:
        raise ValueError(f"{name} is too long for a case ID")
    return name


def _workloads(cases):
    return {case["id"]: case for case in cases}


def bounds_suite(workloads, ladder=None):
    """For each workload, a minimum run (every knob at its cheapest, 540 lines) and a maximum run (every knob at its
    best, native lines)."""
    ladder = read_ladder() if ladder is None else ladder
    suite = CatalogSuite(_workloads(workloads))
    for case in workloads:
        suite.runs.append(SuiteRun(_run_id(case["id"], "min"), case["id"], _bound(ladder, True)))
        suite.runs.append(SuiteRun(_run_id(case["id"], "max"), case["id"], _bound(ladder, False)))
    return suite


def ofat_knobs(ladder=None):
    """Every value of every knob in the ladder but the render height, which gets 540, 720 and native."""
    ladder = read_ladder() if ladder is None else ladder
    knobs = {knob: list(order) for knob, order in ladder.items() if knob != LINES_KNOB}
    if LINES_KNOB in ladder:
        knobs[LINES_KNOB] = [str(lines) for lines in OFAT_LINES]
    return knobs


def ofat_suite(workloads, baseline, knobs):
    """The baseline, then one knob step per run from it. `baseline` and `knobs` are ladder values (text); the
    simulation switches also get a run with all of them on, since they ship together."""
    suite = CatalogSuite(_workloads(workloads))
    switches = [knob for knob in knobs if knob in bc.SIM_SETTINGS]
    for case in workloads:
        suite.runs.append(SuiteRun(_run_id(case["id"], "base"), case["id"], _settings(baseline)))
        for knob, values in knobs.items():
            for value in values:
                if value == baseline.get(knob):
                    continue
                tag = f"{knob.split('.')[-1].replace('_', '-')}-{value}"
                suite.runs.append(SuiteRun(_run_id(case["id"], tag[:30]), case["id"], _settings({**baseline, knob: value})))
        if len(switches) > 1:
            on = {knob: knobs[knob][-1] for knob in switches}
            suite.runs.append(SuiteRun(_run_id(case["id"], "sim-all"), case["id"], _settings({**baseline, **on})))
    return suite


def frame_ms(row, hz):
    return cost.Prediction(row.simulation_cpu_ms, row.render_cpu_ms, row.gpu_ms).frame_ms(hz)


def _usable(row):
    return row.timing_basis == cost.BASIS and row.thermal < cost.THERMAL_SERIOUS


def classify(runs, device, ladder=None, target_fps=TARGET_FPS, floor_fps=FLOOR_FPS):
    """Sort a device's workloads by what its bounds runs say: {workload: "not_recommended" | "no_tuning" | "tune"}.
    The minimum misses the floor (30 FPS at 30 Hz cost): not recommended. The maximum meets the target: no tuning.
    A workload without both bounds is left for the bounds suite."""
    ladder = read_ladder() if ladder is None else ladder
    low, high = _stringify(ladder, True), _stringify(ladder, False)
    rows = {}
    for run in runs:
        if run.device == device and _usable(run):
            rows.setdefault(run.workload, []).append(run)
    result = {}
    for workload, items in sorted(rows.items()):
        lowest = _bound_row(items, low, min)
        highest = _bound_row(items, high, max)
        if lowest is None or highest is None:
            continue
        if frame_ms(lowest, floor_fps) > 1000.0 / floor_fps:
            result[workload] = "not_recommended"
        elif frame_ms(highest, target_fps) <= 1000.0 / target_fps:
            result[workload] = "no_tuning"
        else:
            result[workload] = "tune"
    return result


def _stringify(ladder, lowest):
    """The bounds configuration as a report's `settings` reads: strings, by config name, render height apart."""
    return {knob: (order[-1] if lowest else order[0]) for knob, order in ladder.items() if knob != LINES_KNOB}


def _bound_row(rows, expected, pick):
    """The row at a bound: every other knob as the bound has it (a value the report did not record is not held
    against it), the lowest or highest render height among them."""
    matching = [row for row in rows if all(_same(row.settings.get(knob), value) for knob, value in expected.items()
                                           if knob in row.settings)]
    return pick(matching, key=lambda row: row.render_height) if matching else None


def _same(recorded, wanted):
    if recorded == wanted:
        return True
    recorded, wanted = str(recorded).lower(), str(wanted).lower()
    if wanted in BOOLEAN_TEXT and recorded in BOOLEAN_TEXT:
        return BOOLEAN_TEXT[wanted] == BOOLEAN_TEXT[recorded]
    try:
        return float(recorded) == float(wanted)
    except (TypeError, ValueError):
        return False


def next_suite(runs, device, workloads, baseline, ladder=None, knobs=None, target_fps=TARGET_FPS, floor_fps=FLOOR_FPS):
    """The one-knob suite for the workloads that are neither hopeless nor already fine, or None when none are left.
    The others are listed on the suite's `not_recommended` and `no_tuning` (and returned empty-handed with None
    through classify)."""
    ladder = read_ladder() if ladder is None else ladder
    kinds = classify(runs, device, ladder, target_fps, floor_fps)
    chosen = [case for case in workloads if kinds.get(case["id"], "tune") == "tune"]
    if not chosen:
        return None
    suite = ofat_suite(chosen, baseline, ofat_knobs(ladder) if knobs is None else knobs)
    suite.not_recommended = sorted(name for name, kind in kinds.items() if kind == "not_recommended")
    suite.no_tuning = sorted(name for name, kind in kinds.items() if kind == "no_tuning")
    return suite


def select_workloads(cases, gpu_ms=None, heaviest=None, other=None, multiplayer=MULTIPLAYER_DEFAULTS):
    """Catalog cases by id: the heaviest campaign case (by `gpu_ms`, {case id: milliseconds}, unless named), one other
    campaign case, and the multiplayer cases. Raises ValueError when an id is not in the catalog."""
    by_id = {case["id"]: case for case in cases if "variant_of" not in case}
    campaign = [case["id"] for case in cases if case["kind"] == "gameplay" and "variant_of" not in case]
    if heaviest is None:
        timed = [(gpu_ms.get(name, 0.0), -index, name) for index, name in enumerate(campaign)] if gpu_ms else []
        heaviest = max(timed)[2] if timed and max(timed)[0] > 0 else (campaign[0] if campaign else None)
    if other is None:
        others = [name for name in campaign if name != heaviest]
        other = others[0] if others else None
    wanted = [name for name in (heaviest, other, *multiplayer) if name]
    missing = [name for name in wanted if name not in by_id]
    if missing:
        raise ValueError(f"not in the catalog: {', '.join(missing)}")
    return [by_id[name] for name in wanted]


def heaviest_by_gpu(runs, device=None):
    """{workload: mean GPU milliseconds} over the usable runs, for choosing the most taxing campaign case."""
    totals = {}
    for run in runs:
        if (device is None or run.device == device) and _usable(run) and run.gpu_ms > 0:
            totals.setdefault(run.workload, []).append(run.gpu_ms)
    return {name: sum(values) / len(values) for name, values in totals.items()}


def gpu_times_from_sheet(path, kind="gameplay"):
    """{scene: mean GPU milliseconds} over the finished runs with a GPU time in the benchmark sheet's normalized rows
    (one JSON object a line: `scene`, `kind`, `status`, `gpu_ms_mean`)."""
    totals = {}
    with open(path) as handle:
        for line in handle:
            row = json.loads(line)
            if row.get("status") == "ok" and row.get("kind") == kind and (row.get("gpu_ms_mean") or 0) > 0:
                totals.setdefault(row["scene"], []).append(row["gpu_ms_mean"])
    return {name: sum(values) / len(values) for name, values in totals.items()}


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("bounds", "next"):
        item = commands.add_parser(name, help=f"write the {name} suite's catalog folders")
        item.add_argument("catalog", help="folder with the benchmark catalog and its recordings")
        item.add_argument("out", help="folder to write the suite folders into (one a catalog of at most 16 cases)")
        item.add_argument("--class", dest="klass", default="handheld", choices=CLASSES)
        item.add_argument("--heaviest", help="campaign case id to use as the most taxing")
        item.add_argument("--other", help="the other campaign case id")
        item.add_argument("--multiplayer", nargs="*", default=list(MULTIPLAYER_DEFAULTS))
        item.add_argument("--runs", default=str(cost.RUNS_CSV))
        item.add_argument("--sheet", help="the benchmark sheet's normalized runs (JSON lines) to pick the heaviest case from")
        if name == "next":
            item.add_argument("--device", required=True)
            item.add_argument("--baseline", required=True, help="JSON object of ladder knob to value text")
    options = parser.parse_args(arguments)
    cases = bc.load_catalog(options.catalog)
    runs = cost.read_runs(options.runs)
    times = heaviest_by_gpu(runs)
    if options.sheet:
        times = {**gpu_times_from_sheet(options.sheet), **times}
    workloads = select_workloads(cases, times, options.heaviest, options.other, options.multiplayer)
    print("workloads: " + ", ".join(case["id"] for case in workloads))
    ladder = read_ladder(options.klass)
    if options.command == "bounds":
        suite = bounds_suite(workloads, ladder)
    else:
        suite = next_suite(runs, options.device, workloads, json.loads(options.baseline), ladder)
        if suite is None:
            print("nothing left to tune")
            return 0
        print(f"not recommended: {suite.not_recommended or 'none'}; no tuning needed: {suite.no_tuning or 'none'}")
    for number, part in enumerate(suite.chunks()):
        folder = Path(options.out) / f"{options.command}-{number:02d}"
        print(f"{folder}\t{part.write(folder, options.catalog)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
