#!/usr/bin/env python3
"""Fit a plain-data cost model (CSV of runs, TOML of fits) from benchmark report.json files."""
import argparse
import csv
import json
import statistics
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

COST = Path(__file__).resolve().parent.parent / "profiles" / "cost"
RUNS_CSV = COST / "runs.csv"
MODEL_TOML = COST / "model.toml"
BASIS = "offscreen_uncapped"
THERMAL_SERIOUS = 2
RESOLUTION_KEY = "display.render_lines"
REPORT_SETTINGS = {"render_lines": RESOLUTION_KEY, "anisotropic_filtering": "display.anisotropic_filtering",
                   "shadow_map_size": "display.shadow_map_size", "model_lod": "display.model_lod"}
FIELDS = ["report", "index", "device", "chip", "workload", "settings", "render_width", "render_height",
          "simulation_cpu_ms", "render_cpu_ms", "gpu_ms", "thermal", "timing_basis"]


@dataclass
class Run:
    report: str
    device: str
    chip: str
    workload: str
    settings: dict
    render_width: int
    render_height: int
    simulation_cpu_ms: float
    render_cpu_ms: float
    gpu_ms: float
    thermal: int
    timing_basis: str
    index: int = 0


@dataclass
class Fit:
    device: str
    workload: str
    baseline: dict
    simulation_ms: float
    render_cpu_ms: float
    gpu_fixed_ms: float
    gpu_ms_per_megapixel: float
    knobs: dict = field(default_factory=dict)
    excluded: dict = field(default_factory=dict)
    correction: float = 1.0
    chip: str = ""
    confidence: str = "measured"


@dataclass
class Prediction:
    simulation_tick_ms: float
    render_cpu_ms: float
    gpu_ms: float
    correction: float = 1.0

    def frame_ms(self, target_hz):
        cpu = self.render_cpu_ms + self.simulation_tick_ms * 30 / target_hz
        return max(cpu, self.gpu_ms) * self.correction


def _mean(value):
    """A distribution's mean, or a bare number; None when the report lacks it."""
    if isinstance(value, dict):
        value = value.get("mean")
    return float(value) if isinstance(value, (int, float)) else None


def runs_from_report(report, name=""):
    device = report.get("device", {})
    result = []
    for position, run in enumerate(report.get("runs", [])):
        if run.get("status") not in (None, "complete", "completed", "ok"):
            continue
        settings = {REPORT_SETTINGS.get(key, key): str(value) for key, value in run.get("settings", {}).items()
                    if not key.endswith("_requested")}
        size = run.get("render_size") or [0, 0]
        thermal = run.get("thermal", {}).get("worst", -1) if isinstance(run.get("thermal"), dict) else -1
        result.append(Run(report=name or str(report.get("code", "")), device=device.get("model", ""),
                          chip=device.get("chip", ""), workload=run.get("scene", ""), settings=settings,
                          render_width=int(size[0]), render_height=int(size[1]),
                          simulation_cpu_ms=_mean(run.get("simulation_cpu_ms")) or 0.0,
                          render_cpu_ms=_mean(run.get("render_cpu_ms")) or 0.0,
                          gpu_ms=_mean(run.get("gpu_ms")) or 0.0, thermal=int(thermal),
                          timing_basis=run.get("timing_basis", ""), index=int(run.get("index", position))))
    return result


def read_runs(path):
    if not Path(path).exists():
        return []
    with open(path, newline="") as handle:
        return [Run(report=row["report"], index=int(row["index"]), device=row["device"], chip=row["chip"],
                    workload=row["workload"], settings=json.loads(row["settings"]),
                    render_width=int(row["render_width"]), render_height=int(row["render_height"]),
                    simulation_cpu_ms=float(row["simulation_cpu_ms"]), render_cpu_ms=float(row["render_cpu_ms"]),
                    gpu_ms=float(row["gpu_ms"]), thermal=int(row["thermal"]), timing_basis=row["timing_basis"])
                for row in csv.DictReader(handle)]


def write_runs(path, runs):
    """Append runs to the CSV, replacing rows with the same (report, index)."""
    merged = {(run.report, run.index): run for run in read_runs(path)}
    merged.update({(run.report, run.index): run for run in runs})
    with open(path, "w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(FIELDS)
        for (_, _), run in sorted(merged.items()):
            writer.writerow([run.report, run.index, run.device, run.chip, run.workload,
                             json.dumps(run.settings, sort_keys=True), run.render_width, run.render_height,
                             run.simulation_cpu_ms, run.render_cpu_ms, run.gpu_ms, run.thermal, run.timing_basis])


def _megapixels(run):
    return run.render_width * run.render_height / 1e6


def _line_fit(points):
    """Least squares gpu = fixed + slope * megapixels; one resolution gives a pure per-pixel cost."""
    xs = sorted({round(x, 9) for x, _ in points})
    if len(xs) < 2:
        x, y = points[0][0], statistics.mean(y for _, y in points)
        return 0.0, y / x if x else 0.0
    mean_x = statistics.mean(x for x, _ in points)
    mean_y = statistics.mean(y for _, y in points)
    slope = (sum((x - mean_x) * (y - mean_y) for x, y in points) /
             sum((x - mean_x) ** 2 for x, _ in points))
    return mean_y - slope * mean_x, slope


def _base_settings(rows):
    """Each knob at its most common value, so the baseline is the configuration most runs share."""
    keys = {key for row in rows for key in row.settings} - {RESOLUTION_KEY}
    return {key: Counter(row.settings[key] for row in rows if key in row.settings).most_common(1)[0][0]
            for key in sorted(keys)}


def _same_knobs(row, baseline):
    return all(row.settings.get(key) == value for key, value in baseline.items())


def fit(runs):
    groups = defaultdict(list)
    for run in runs:
        groups[(run.device, run.workload)].append(run)
    fits = []
    for (device, workload), rows in sorted(groups.items()):
        excluded = Counter()
        kept = []
        for row in rows:
            if row.thermal >= THERMAL_SERIOUS:
                excluded["thermal"] += 1
            elif row.timing_basis != BASIS:
                excluded["timing_basis"] += 1
            else:
                kept.append(row)
        if not kept:
            fits.append(Fit(device, workload, {}, 0.0, 0.0, 0.0, 0.0, excluded=dict(excluded),
                            chip=rows[0].chip, confidence="none"))
            continue
        baseline = _base_settings(kept)
        base_rows = [row for row in kept if _same_knobs(row, baseline)]
        usable = [row for row in base_rows if row.gpu_ms > 0]
        excluded["gpu_zero"] += len(base_rows) - len(usable)
        fixed, slope = _line_fit([(_megapixels(row), row.gpu_ms) for row in usable]) if usable else (0.0, 0.0)
        gpu_at = {row.render_height: statistics.mean(r.gpu_ms for r in usable if r.render_height == row.render_height)
                  for row in usable}
        cpu_at = {row.render_height: statistics.mean(r.render_cpu_ms for r in base_rows if r.render_height == row.render_height)
                  for row in base_rows}
        ratios = defaultdict(lambda: ([], []))
        for row in kept:
            changed = [key for key, value in baseline.items() if row.settings.get(key) != value]
            if len(changed) != 1:
                continue
            knob = changed[0]
            gpu, cpu = ratios[(knob, row.settings[knob])]
            if row.gpu_ms > 0 and gpu_at.get(row.render_height):
                gpu.append(row.gpu_ms / gpu_at[row.render_height])
            elif row.gpu_ms <= 0:
                excluded["gpu_zero"] += 1
            if cpu_at.get(row.render_height):
                cpu.append(row.render_cpu_ms / cpu_at[row.render_height])
        knobs = defaultdict(dict)
        for (knob, value), (gpu, cpu) in sorted(ratios.items()):
            knobs[knob][value] = (statistics.median(cpu) if cpu else 1.0, statistics.median(gpu) if gpu else 1.0)
        fits.append(Fit(device=device, workload=workload, baseline=baseline,
                        simulation_ms=statistics.mean(row.simulation_cpu_ms for row in base_rows) if base_rows else 0.0,
                        render_cpu_ms=statistics.mean(row.render_cpu_ms for row in base_rows) if base_rows else 0.0,
                        gpu_fixed_ms=fixed, gpu_ms_per_megapixel=slope, knobs=dict(knobs),
                        excluded={key: count for key, count in excluded.items() if count}, chip=kept[0].chip))
    return fits


def predict(fit_, settings, width, height):
    cpu_factor = gpu_factor = 1.0
    for knob, value in settings.items():
        if knob == RESOLUTION_KEY or fit_.baseline.get(knob) == value:
            continue
        cpu, gpu = fit_.knobs.get(knob, {}).get(value, (1.0, 1.0))
        cpu_factor *= cpu
        gpu_factor *= gpu
    gpu = (fit_.gpu_fixed_ms + fit_.gpu_ms_per_megapixel * width * height / 1e6) * gpu_factor
    return Prediction(simulation_tick_ms=fit_.simulation_ms, render_cpu_ms=fit_.render_cpu_ms * cpu_factor,
                      gpu_ms=gpu, correction=fit_.correction)


def _q(text):
    return json.dumps(str(text), ensure_ascii=True)


def write_model(path, fits):
    lines = ["# Generated by tools/profile_cost_model.py fit. Do not edit by hand.", "schema = 1", ""]
    for item in fits:
        lines += ["[[fit]]", f"device = {_q(item.device)}", f"workload = {_q(item.workload)}", f"chip = {_q(item.chip)}",
                  f"confidence = {_q(item.confidence)}", f"simulation_ms = {item.simulation_ms!r}",
                  f"render_cpu_ms = {item.render_cpu_ms!r}", f"gpu_fixed_ms = {item.gpu_fixed_ms!r}",
                  f"gpu_ms_per_megapixel = {item.gpu_ms_per_megapixel!r}", f"correction = {item.correction!r}",
                  "baseline = {" + ", ".join(f"{_q(k)} = {_q(v)}" for k, v in sorted(item.baseline.items())) + "}",
                  "excluded = {" + ", ".join(f"{_q(k)} = {v}" for k, v in sorted(item.excluded.items())) + "}"]
        for knob, values in sorted(item.knobs.items()):
            for value, (cpu, gpu) in sorted(values.items()):
                lines += ["[[fit.knob]]", f"name = {_q(knob)}", f"value = {_q(value)}",
                          f"cpu_factor = {cpu!r}", f"gpu_factor = {gpu!r}"]
        lines.append("")
    Path(path).write_text("\n".join(lines))


def read_model(path):
    import tomllib
    with open(path, "rb") as handle:
        table = tomllib.load(handle)
    fits = []
    for entry in table.get("fit", []):
        knobs = defaultdict(dict)
        for knob in entry.get("knob", []):
            knobs[knob["name"]][knob["value"]] = (knob["cpu_factor"], knob["gpu_factor"])
        fits.append(Fit(device=entry["device"], workload=entry["workload"], baseline=dict(entry["baseline"]),
                        simulation_ms=entry["simulation_ms"], render_cpu_ms=entry["render_cpu_ms"],
                        gpu_fixed_ms=entry["gpu_fixed_ms"], gpu_ms_per_megapixel=entry["gpu_ms_per_megapixel"],
                        knobs=dict(knobs), excluded=dict(entry["excluded"]), correction=entry["correction"],
                        chip=entry.get("chip", ""), confidence=entry.get("confidence", "measured")))
    return fits


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    ingest = commands.add_parser("ingest", help="append runs from report.json files to profiles/cost/runs.csv")
    ingest.add_argument("reports", nargs="+")
    commands.add_parser("fit", help="fit profiles/cost/model.toml from profiles/cost/runs.csv")
    options = parser.parse_args(arguments)
    if options.command == "ingest":
        runs = []
        for name in options.reports:
            runs += runs_from_report(json.loads(Path(name).read_text()), Path(name).parent.name or Path(name).stem)
        write_runs(RUNS_CSV, runs)
        print(f"ingested {len(runs)} runs")
    else:
        fits = fit(read_runs(RUNS_CSV))
        write_model(MODEL_TOML, fits)
        for item in fits:
            print(f"{item.device} {item.workload}: excluded {item.excluded or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
