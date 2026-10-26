#!/usr/bin/env python3
"""Export the public benchmark summary as tidy CSV for the results spreadsheet."""
from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import statistics
import sys

try:
    from .results import PORTS, SCHEMA, _record_order, _scenarios, _valid_rounds
except ImportError:
    from results import PORTS, SCHEMA, _record_order, _scenarios, _valid_rounds

ROOT = Path(__file__).resolve().parents[2]
SUMMARY = ROOT / "tools/perf_compare/summary.json"

# One row per measured set and scenario window. The order is part of the contract: the spreadsheet's
# per-port tabs and the README tab refer to columns by position and name.
COLUMNS = (
    "port", "port_name", "version", "label", "build", "source", "release_kind", "artifact", "artifact_sha256",
    "host", "platform", "reference_box", "machine", "chip", "memory_gb", "os", "browser", "date", "method",
    "suite", "window", "scenario", "delay_s", "frame_mode", "quality_preset", "warmup_s",
    "resolution", "cap_fps", "display_hz", "rounds",
    "fps_avg", "fps_min", "fps_max", "fps_1pct_low", "p50_ms", "p95_ms", "p99_ms", "p99_ms_max", "worst_ms_max", "on_time_pct",
    "thermal", "status", "notes",
)

NUMERIC = ("resolution", "cap_fps", "display_hz", "fps_avg", "fps_min", "fps_max", "fps_1pct_low", "p50_ms", "p95_ms",
           "p99_ms", "p99_ms_max", "worst_ms_max", "on_time_pct", "thermal")

SWEEP_COLUMNS = ("device", "build", "workload", "resolution", "configuration", "settings_hash", "trials",
                 "fps_min", "fps_max", "timing_basis", "thermal", "report")

DESCRIPTIONS = {
    "port": "Port identifier (visr, opence-macos, opence-web, corece, ...). Per-port tabs filter on it.",
    "port_name": "Public port name.",
    "version": "Product version as the app reports it, or the release tag. main marks a pre-release VISR build.",
    "label": "Port name and version, used as the chart series name.",
    "build": "Bundle build number or hosted build identifier.",
    "source": "Source commit of the measured build (a release tag's commit for other ports).",
    "release_kind": "release, pre-release main build, or hosted web build.",
    "artifact": "What was measured: a release download, an ad hoc build of a tag, or a hosted page.",
    "artifact_sha256": "SHA-256 of the measured artifact.",
    "host": "Chip and machine model, for example M4 Mac mini.",
    "platform": "Mac, iPhone/iPad, Apple TV or Vision Pro, from the machine model.",
    "reference_box": "true for the platform's reference device, whose rows feed the page's headline chart.",
    "machine": "Machine model.",
    "chip": "Apple silicon chip.",
    "memory_gb": "Installed memory in GB.",
    "os": "Operating system version.",
    "browser": "Browser and version for web builds; empty for native apps.",
    "date": "Date the set was measured (UTC).",
    "method": "Measurement method: metalperftrace on-glass intervals, or Safari requestAnimationFrame callbacks.",
    "suite": "Workload suite: campaign, multiplayer or co-op. Every row measured so far is campaign.",
    "window": "Sampling window: the scenario, plus the delay after warm-up when it is not zero.",
    "scenario": "Level scenario: a10 or b30.",
    "delay_s": "Seconds added after warm-up before sampling starts.",
    "frame_mode": "capped (60 fps cap) or uncapped (unlimited, vsync off).",
    "quality_preset": "defaults (each app's own) or matched-stock (the documented stock options).",
    "warmup_s": "Warm-up seconds after player readiness.",
    "resolution": "Drawable size in pixels.",
    "cap_fps": "Requested frame cap; empty for uncapped runs.",
    "display_hz": "Display refresh rate.",
    "rounds": "Valid rounds behind the row.",
    "fps_avg": "Mean over rounds of the average rate (inverse of the mean presentation interval).",
    "fps_min": "Lowest round's average rate.",
    "fps_max": "Highest round's average rate.",
    "fps_1pct_low": "Mean over rounds of 1000 divided by the 99th percentile interval: the rate of the slowest 1% of frames.",
    "p50_ms": "Mean over rounds of the median presentation interval.",
    "p95_ms": "Mean over rounds of the 95th percentile interval.",
    "p99_ms": "Mean over rounds of the 99th percentile interval.",
    "p99_ms_max": "Highest round's 99th percentile interval.",
    "worst_ms_max": "Longest single interval in any round.",
    "on_time_pct": "Mean share of intervals within 5% of the display rate; empty for uncapped rows.",
    "thermal": "System thermal state at the start and end of the runs.",
    "status": "ok, incomplete (fewer than three rounds), unavailable (could not run), not_supported (the port lacks the feature the suite needs, for example split screen for co-op), or rejected (one row per rejected attempt, with its reason in notes).",
    "notes": "Why a set is unavailable or incomplete.",
}

# The device each platform's headline chart is drawn from. Apple TV has none yet.
REFERENCE_BOXES = {"Mac": "M6 Mac mini", "iPhone/iPad": "M1 iPad Pro", "Apple TV": None, "Vision Pro": "M2 Vision Pro"}


def platform_of(model: str) -> str:
    lowered = model.lower()
    for needle, name in (("vision", "Vision Pro"), ("apple tv", "Apple TV"), ("ipad", "iPhone/iPad"),
                         ("iphone", "iPhone/iPad")):
        if needle in lowered:
            return name
    return "Mac"


_MACHINE = re.compile(r"^(?P<model>.*?) \((?P<chip>[^,()]+), (?P<memory>\d+) GB\)$")


def _split_machine(text: str) -> tuple[str, str, str]:
    match = _MACHINE.match(text)
    if not match:
        return text, "", ""
    return match["model"], match["chip"], match["memory"]


def _mean(valid, key):
    return statistics.fmean(run["metrics"][key] for run in valid)


def _number(value, places=2):
    return "" if value is None else f"{value:.{places}f}"


def rows(summary: dict) -> list[dict[str, str]]:
    """One row per measured record and scenario, in the summary's page order."""
    if summary.get("schema", SCHEMA) != SCHEMA:
        raise ValueError("summary schema mismatch")
    out = []
    for record in sorted(summary.get("records", []), key=_record_order):
        port = str(record.get("port", ""))
        name = PORTS.get(port, port)
        version = str(record.get("version") or "")
        model, chip, memory = _split_machine(str(record.get("machine", "")))
        platform = platform_of(model)
        host = f"{chip} {model}".strip()
        delay = int(record.get("native_delay_seconds") or 0)
        for scenario in _scenarios(record):
            valid = _valid_rounds(record, scenario)
            row = dict.fromkeys(COLUMNS, "")
            row.update(
                port=port, port_name=name, version=version, label=f"{name} {version}".strip(),
                build=record.get("build") or "", source=record.get("source") or "",
                release_kind=record.get("release_kind") or "", artifact=record.get("artifact") or "",
                artifact_sha256=record.get("sha256") or "",
                host=host, platform=platform,
                reference_box="true" if REFERENCE_BOXES.get(platform) == host else "false", machine=model, chip=chip, memory_gb=memory,
                os=record.get("os", ""), browser=record.get("browser") or "", date=record.get("date", ""),
                method=record.get("method", ""), window=scenario if not delay else f"{scenario} +{delay} s",
                scenario=scenario, suite=record.get("suite") or "campaign", delay_s=delay, frame_mode=record.get("frame_mode", "capped"),
                quality_preset=record.get("quality_preset", "defaults"), warmup_s=record.get("warmup_seconds", 15),
                rounds=len(valid),
            )
            if valid:
                fps = [run["metrics"]["fps"] for run in valid]
                row.update(
                    resolution=", ".join(sorted({str(run.get("resolution", "")) for run in valid})),
                    cap_fps="" if record.get("frame_mode") == "uncapped"
                    else ", ".join(sorted({str(run.get("cap", "")) for run in valid})),
                    display_hz=", ".join(sorted({str(run.get("display_hz", "")) for run in valid})),
                    fps_avg=_number(statistics.fmean(fps)), fps_min=_number(min(fps)), fps_max=_number(max(fps)),
                    fps_1pct_low=_number(statistics.fmean(1000 / run["metrics"]["p99_ms"] for run in valid)),
                    p50_ms=_number(_mean(valid, "p50_ms")), p95_ms=_number(_mean(valid, "p95_ms")),
                    p99_ms=_number(_mean(valid, "p99_ms")),
                    p99_ms_max=_number(max(run["metrics"]["p99_ms"] for run in valid)),
                    worst_ms_max=_number(max(run["metrics"]["worst_ms"] for run in valid)),
                    on_time_pct="" if record.get("frame_mode") == "uncapped" else _number(_mean(valid, "on_time_pct"), 1),
                    thermal=", ".join(sorted({str(run.get("thermal", "")) for run in valid})),
                )
            status = str(record.get("status", "unknown"))
            if status == "not_supported":
                pass  # the port cannot run this suite at all; distinct from unavailable and not measured
            elif not valid:
                status = "unavailable"
            elif len(valid) < 3:
                status = "incomplete"
            row["status"] = status
            row["notes"] = str(record.get("reason") or "")
            out.append(row)
            # Rejected attempts never enter the aggregates; each stays visible as its own row.
            for attempt in record.get("rejected", []):
                if str(attempt.get("scenario")) == scenario:
                    gone = {**row, **dict.fromkeys(NUMERIC, ""), "rounds": 0, "status": "rejected",
                            "notes": f"round {attempt.get('round', '?')}: {attempt.get('reason', 'unknown')}"}
                    out.append(gone)
    return out


def sweep_rows(summary: dict) -> list[dict[str, str]]:
    """Device sweep reports, with the reported rate range split into two numeric columns."""
    out = []
    for item in summary.get("sweep", []):
        low, _, high = str(item.get("fps", "")).replace("-", "–").partition("–")
        row = {key: str(item.get(key, "")) for key in SWEEP_COLUMNS}
        row["fps_min"], row["fps_max"] = low, high or low
        out.append(row)
    out.sort(key=lambda row: tuple(row[key] for key in SWEEP_COLUMNS))
    return out


def _column_letter(index: int) -> str:
    letters = ""
    index += 1
    while index:
        index, remainder = divmod(index - 1, 26)
        letters = chr(65 + remainder) + letters
    return letters


def layout(summary: dict) -> dict:
    """The spreadsheet's tabs: the data tabs from CSV files and formula tabs built over `results`."""
    last = _column_letter(len(COLUMNS) - 1)
    ports = list(dict.fromkeys(row["port"] for row in rows(summary)))
    tabs = [{"name": "README", "kind": "table", "header": ["column", "tab", "meaning"],
             "rows": [[name, "results", DESCRIPTIONS[name]] for name in COLUMNS]
             + [[name, "device-sweep", ""] for name in SWEEP_COLUMNS],
             "note": "results is the only tab to edit. The per-port tabs are read-only views."},
            {"name": "results", "kind": "csv", "file": "results.csv", "columns": list(COLUMNS)}]
    # Each port tab repeats the header row, then filters the data rows on the port column.
    tabs.extend({"name": port, "kind": "formula",
                 "cell": "A1",
                 "formula": f'={{results!A1:{last}1; FILTER(results!A2:{last}, results!A2:A="{port}")}}'}
                for port in ports)
    if summary.get("sweep"):
        tabs.append({"name": "device-sweep", "kind": "csv", "file": "sweep.csv", "columns": list(SWEEP_COLUMNS)})
    # Sheets would turn all-digit or exponent-looking values into numbers and drop trailing zeros.
    text_columns = ["version", "build", "source", "artifact_sha256", "window", "os"]
    return {"plain_text_columns": {"results": text_columns}, "sheet_id": "1tuTJpOxUTbqgzOSDSW-nSvWKL8HkZMVucvNRUIee0t4", "tabs": tabs}


def to_csv(columns, table) -> str:
    class Buffer:
        parts: list[str]

        def __init__(self):
            self.parts = []

        def write(self, text):
            self.parts.append(text)

    buffer = Buffer()
    writer = csv.DictWriter(buffer, fieldnames=list(columns), lineterminator="\n")
    writer.writeheader()
    writer.writerows(table)
    return "".join(buffer.parts)


def main() -> int:
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", type=Path, default=SUMMARY)
    parser.add_argument("--output", type=Path, required=True, help="results.csv to write")
    parser.add_argument("--sweep-output", type=Path, help="sweep.csv to write")
    parser.add_argument("--layout-output", type=Path, help="layout.json to write")
    args = parser.parse_args()
    summary = json.loads(args.summary.read_text())
    args.output.write_text(to_csv(COLUMNS, rows(summary)))
    if args.sweep_output:
        args.sweep_output.write_text(to_csv(SWEEP_COLUMNS, sweep_rows(summary)))
    if args.layout_output:
        args.layout_output.write_text(json.dumps(layout(summary), indent=2, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
