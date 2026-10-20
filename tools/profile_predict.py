#!/usr/bin/env python3
"""Predict cost-model fits for devices without measurements, from device facts and the nearest measured fit."""
import argparse
import csv
import dataclasses
import re
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

if __package__:
    from . import profile_cost_model as cost
else:
    import profile_cost_model as cost

FACTS_CSV = cost.COST / "device-facts.csv"
SPEEDS_TOML = cost.COST / "chip-speeds.toml"
FACT_FIELDS = ["model", "family", "chip", "identifiers", "gpu_cores", "native_width", "native_height",
               "max_refresh_hz", "tested_count", "source"]
FAMILY = re.compile(r"(A\d+[XZ]?|M\d+)")


@dataclass
class Device:
    model: str
    chip: str
    family: str
    gpu_cores: int = 0
    native_width: int = 0
    native_height: int = 0
    max_refresh_hz: int = 0
    owned: bool = False
    identifiers: tuple = ()


def family_of(chip):
    match = FAMILY.match(chip.strip())
    return match.group(1) if match else ""


def generation(family):
    """A-series number, with M1 to M5 counted as generations 14 to 18 (the same ordering as chip-speeds.toml)."""
    number = int(re.search(r"\d+", family).group())
    return number if family.startswith("A") else 13 + number


def load_speeds(path=SPEEDS_TOML):
    with open(path, "rb") as handle:
        return tomllib.load(handle)


def _integer(text):
    return int(text) if text and text.strip().isdigit() else 0


def read_devices(path=FACTS_CSV):
    with open(path, newline="") as handle:
        return [Device(model=row["model"], chip=row["chip"], family=row["family"],
                       gpu_cores=_integer(row["gpu_cores"]), native_width=_integer(row["native_width"]),
                       native_height=_integer(row["native_height"]), max_refresh_hz=_integer(row["max_refresh_hz"]),
                       owned=_integer(row["tested_count"]) > 0,
                       identifiers=tuple(part for part in row["identifiers"].split(";") if part))
                for row in csv.DictReader(handle)]


def _is(fit_, device):
    return fit_.device == device.model or fit_.device in device.identifiers


def anchor_for(device, fits, devices=(), speeds=None, workload=None):
    """Return (fit, (gpu_scale, cpu_scale), confidence): the nearest measured fit for the device.

    A scale multiplies the anchor's time to estimate this device. Confidence is "measured" for the
    device's own fit, "inferred" for a fit from the same chip family, and "low" otherwise."""
    speeds = speeds or load_speeds()
    candidates = [f for f in fits if f.confidence != "none" and (workload is None or f.workload == workload)]
    if not candidates:
        raise ValueError("no measured fit to anchor on")
    for item in candidates:
        if _is(item, device):
            return item, (1.0, 1.0), "measured"
    known = {name: d for d in devices for name in (d.model, *d.identifiers)}

    def anchor_device(item):
        found = known.get(item.device)
        return found or Device(item.device, item.chip, family_of(item.chip))

    def distance(item):
        return abs(generation(anchor_device(item).family) - generation(device.family))

    anchor = min(candidates, key=distance)
    source = anchor_device(anchor)
    gpu = (source.gpu_cores / device.gpu_cores) if source.gpu_cores and device.gpu_cores else 1.0
    gpu *= speeds[source.family]["gpu_speed"] / speeds[device.family]["gpu_speed"]
    cpu = speeds[source.family]["cpu_speed"] / speeds[device.family]["cpu_speed"]
    return anchor, (gpu, cpu), "inferred" if source.family == device.family else "low"


def predicted_fit(device, fits, devices=(), speeds=None, workload=None):
    anchor, (gpu, cpu), confidence = anchor_for(device, fits, devices, speeds, workload)
    if confidence == "measured":
        return anchor
    return dataclasses.replace(
        anchor, device=device.model, chip=device.chip, confidence=confidence,
        simulation_ms=anchor.simulation_ms * cpu, render_cpu_ms=anchor.render_cpu_ms * cpu,
        gpu_fixed_ms=anchor.gpu_fixed_ms * gpu, gpu_ms_per_megapixel=anchor.gpu_ms_per_megapixel * gpu,
        excluded={}, correction=1.0)


def fit_chip_speeds(fits, devices, speeds=None):
    """Refit per-family speeds from measured fits once two or more families have one.

    Cost at one megapixel times GPU cores gives a per-family number; speed is its inverse, scaled so the
    lowest measured generation keeps its current value. Unmeasured families keep their assumed rows."""
    speeds = {name: dict(row) for name, row in (speeds or load_speeds()).items()}
    known = {name: d for d in devices for name in (d.model, *d.identifiers)}
    by_workload = {}
    for item in fits:
        owner = known.get(item.device)
        if owner and item.confidence != "none":
            by_workload.setdefault(item.workload, {}).setdefault(owner.family, []).append((item, owner))
    if not by_workload:
        return speeds
    families = max(by_workload.values(), key=len)
    if len(families) < 2:
        return speeds

    def mean(values):
        return sum(values) / len(values)

    gpu_cost = {f: mean([(i.gpu_fixed_ms + i.gpu_ms_per_megapixel) * (o.gpu_cores or 1) for i, o in rows if
                         i.gpu_fixed_ms + i.gpu_ms_per_megapixel > 0] or [0]) for f, rows in families.items()}
    cpu_cost = {f: mean([i.render_cpu_ms + i.simulation_ms for i, _ in rows]) for f, rows in families.items()}
    reference = min(families, key=generation)
    for family in families:
        if gpu_cost[family] > 0 and gpu_cost[reference] > 0:
            speeds[family]["gpu_speed"] = round(speeds[reference]["gpu_speed"] * gpu_cost[reference] / gpu_cost[family], 4)
            speeds[family]["basis"] = "fit"
        if cpu_cost[family] > 0 and cpu_cost[reference] > 0:
            speeds[family]["cpu_speed"] = round(speeds[reference]["cpu_speed"] * cpu_cost[reference] / cpu_cost[family], 4)
            speeds[family]["basis"] = "fit"
    return speeds


def facts_from_coverage(coverage_csv, existing=None):
    """Device facts from the Sheet's Model coverage tab, keeping any value already filled in by hand.

    gpu_cores, native_width, native_height and max_refresh_hz are not in the tab and stay blank until a
    source is added."""
    with open(coverage_csv, newline="") as handle:
        rows = list(csv.reader(handle))
    start = next(i for i, row in enumerate(rows) if row and row[0] == "Family")
    names = rows[start]
    kept = {row["model"]: row for row in existing or []}
    result = []
    for values in rows[start + 1:]:
        row = dict(zip(names, values))
        if not row.get("Model"):
            continue
        identifiers = [] if row["Hardware identifiers"].startswith("Not verified") else \
            [part.strip() for part in re.split(r",\s+", row["Hardware identifiers"]) if part.strip()]
        fact = {"model": row["Model"], "family": family_of(row["Chip / SoC"]), "chip": row["Chip / SoC"],
                "identifiers": ";".join(identifiers), "gpu_cores": "", "native_width": "", "native_height": "",
                "max_refresh_hz": "", "tested_count": row["Unique physical devices tested"],
                "source": f"Sheet Model coverage tab as of {row['As of']}; chip: {row['Chip source']}"}
        for key, value in kept.get(row["Model"], {}).items():
            if key in ("gpu_cores", "native_width", "native_height", "max_refresh_hz") and value:
                fact[key] = value
        result.append(fact)
    return result


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    facts = commands.add_parser("facts", help="rewrite cost-model/device-facts.csv from a Model coverage CSV export")
    facts.add_argument("coverage_csv")
    options = parser.parse_args(arguments)
    existing = list(csv.DictReader(open(FACTS_CSV, newline=""))) if FACTS_CSV.exists() else []
    rows = facts_from_coverage(options.coverage_csv, existing)
    with open(FACTS_CSV, "w", newline="") as handle:
        writer = csv.DictWriter(handle, FACT_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {len(rows)} devices")
    return 0


if __name__ == "__main__":
    sys.exit(main())
