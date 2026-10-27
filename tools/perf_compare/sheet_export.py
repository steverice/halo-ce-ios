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

# One row per measured set and scenario window. The sheet's frozen first column is `label`; the per-port tabs
# filter on column B (`port`). Everything that reads the snapshot does so by header name.
COLUMNS = (
    "label", "port", "port_name", "version", "build", "source", "release_kind", "artifact", "artifact_sha256",
    "host", "platform", "reference_box", "machine", "chip", "memory_gb", "os", "browser", "date", "method",
    "suite", "window", "scenario", "delay_s", "frame_mode", "quality_preset", "warmup_s",
    "resolution", "cap_fps", "display_hz", "rounds",
    "fps_avg", "fps_min", "fps_max", "fps_1pct_low", "p50_ms", "p95_ms", "p99_ms", "p99_ms_max", "worst_ms_max", "on_time_pct",
    "thermal", "status", "notes", "release_url", "gpu_p50_ms", "gpu_p90_ms",
)

NUMERIC = ("gpu_p50_ms", "gpu_p90_ms", "resolution", "cap_fps", "display_hz", "fps_avg", "fps_min", "fps_max", "fps_1pct_low", "p50_ms", "p95_ms",
           "p99_ms", "p99_ms_max", "worst_ms_max", "on_time_pct", "thermal")

# Where each port publishes a build: its release page (never a download), or the hosted page for a web build.
# `{tag}` is the version with a leading v, `{bare}` the version without one, `{build}` the build identifier.
RELEASE_PAGES = {
    "visr": "https://github.com/steverice/visr/releases/tag/visr-v{bare}",
    "opence-macos": "https://github.com/channelramble/OpenCE-macOS/releases/tag/{tag}",
    "opence-web": "https://halocombatevolved.com/",
    "corece": "https://github.com/mastercheese-dev/CoreCE/releases/tag/build-{build}",
    "chupathingy": "https://github.com/ChupathingyCE/chupathingyce/releases/tag/{tag}",
    "halo-og": "https://github.com/pfista/halo-og/releases/tag/{tag}",
    "beeman": "https://github.com/bmanturner/halo-ce-universal/releases/tag/apple-v{bare}",
    "chupathingyce-ios": "https://github.com/JizzyRivers/chupathingyce-ios/releases/tag/{tag}",
    "halopad": "https://github.com/chrissotraidis/projectreach/releases/tag/{tag}",
    "snailbbq": "https://github.com/SnailBBQ/Halo-CE-IOS/releases/tag/{tag}",
    "master-chef": "https://github.com/mitchaiet/master-chef/releases/tag/{tag}",
}


def release_url(port: str, version: str, build: str = "") -> str:
    """The release page for a build, or an empty string for a port without a known page."""
    template = RELEASE_PAGES.get(port)
    if not template or ("{build}" in template and not build):
        return ""
    bare = version.removeprefix("apple-").removeprefix("visr-").removeprefix("v")
    if port != "opence-web" and not bare[:1].isdigit():
        return ""  # a pre-release build such as VISR main has no release page
    return template.format(tag="v" + bare, bare=bare, build=build)


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
    "method": "Measurement method: metalperftrace on-glass intervals (Mac), Instruments Metal System Trace displayed-surfaces intervals (iPhone/iPad), the app's own benchmark report or telemetry (Vision Pro), or Safari requestAnimationFrame callbacks (web).",
    "suite": "Workload suite: campaign, multiplayer (hosted Slayer on Chill Out and Blood Gulch, one scripted-bot player) or co-op (two-player split screen). Multiplayer and co-op windows are equivalent workloads across ports, not identical replays.",
    "window": "Sampling window: the scenario, plus the delay after warm-up when it is not zero.",
    "scenario": "Level scenario: a10 or b30.",
    "delay_s": "Seconds added after warm-up before sampling starts.",
    "frame_mode": "capped (the frame cap on, as on iPhone, iPad, Apple TV and Vision Pro, which are presented runs at the device's display rate, or the 60 fps cap on a Mac) or uncapped (unlimited, vsync off, Mac only).",
    "quality_preset": "defaults (each app's own) or matched-stock (the documented stock options).",
    "warmup_s": "Warm-up seconds after player readiness.",
    "resolution": "Drawable size in pixels. Halo OG always renders at the display's full size.",
    "cap_fps": "Frame cap in force; empty for uncapped runs. master-chef on Vision Pro is fixed at 30 fps as shipped.",
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
    "release_url": "The build's release page on the port's own site, or the hosted page for a web build. Never a download link.",
    "gpu_p50_ms": "Median GPU time of a presented frame (metalperftrace On-GPU Time), mean over the valid rounds. Empty when the trace has no per-frame GPU times.",
    "gpu_p90_ms": "90th-percentile GPU time of a presented frame, mean over the valid rounds. Empty when the trace has no per-frame GPU times.",
    "notes": "Why a set is unavailable or incomplete.",
}

# The device each platform's headline chart is drawn from.
REFERENCE_BOXES = {"Mac": "M6 Mac mini", "iPhone/iPad": "M1 iPad Pro", "Apple TV": "Apple TV HD", "Vision Pro": "M2 Vision Pro"}


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


RELEASE_KINDS = ("release", "hosted web build (release channel)")


def is_release(record: dict) -> bool:
    """Public data holds released builds only: a tagged release, or a hosted build on a release channel."""
    return str(record.get("release_kind") or "") in RELEASE_KINDS


def rows(summary: dict) -> list[dict[str, str]]:
    """One row per measured record and scenario, in the summary's page order."""
    if summary.get("schema", SCHEMA) != SCHEMA:
        raise ValueError("summary schema mismatch")
    out = []
    for record in sorted((r for r in summary.get("records", []) if is_release(r)), key=_record_order):
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
                rounds=len(valid), release_url=release_url(port, version, record.get("build") or ""),
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
                    gpu_p50_ms=_gpu(valid, "gpu_p50_ms"), gpu_p90_ms=_gpu(valid, "gpu_p90_ms"),
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


def _gpu(valid: list[dict], key: str) -> str:
    """Mean of a GPU frame-time metric over the valid rounds, empty unless every round has it."""
    if not all(key in run["metrics"] for run in valid):
        return ""
    return _number(statistics.fmean(run["metrics"][key] for run in valid), 2)


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
    tabs = [{"name": "README", "kind": "table", "header": ["column", "meaning"],
             "rows": [[name, DESCRIPTIONS[name]] for name in COLUMNS],
             "note": "results is the only tab to edit. The per-port tabs are read-only views."},
            {"name": "charts", "kind": "summary",
             "note": "Per platform, for campaign sets on the reference box with status ok: the geometric mean over every "
                     "window of fps_avg and fps_1pct_low, by label. Mac uses uncapped, matched-stock runs; the other platforms "
                     "use runs presented at the display rate or the profile's frame cap, with default settings. The page uses the same rules."},
            {"name": "results", "kind": "csv", "file": "results.csv", "columns": list(COLUMNS)}]
    # Each port tab repeats the header row, then filters the data rows on the port column.
    tabs.extend({"name": port, "kind": "formula",
                 "cell": "A1",
                 "formula": f'={{results!A1:{last}1; FILTER(results!A2:{last}, results!B2:B="{port}")}}'}
                for port in ports)
    # Sheets would turn all-digit or exponent-looking values into numbers and drop trailing zeros.
    text_columns = ["version", "build", "source", "artifact_sha256", "window", "os"]
    return {"plain_text_columns": {"results": text_columns}, "sheet_id": "1tuTJpOxUTbqgzOSDSW-nSvWKL8HkZMVucvNRUIee0t4", "tabs": tabs}


def readme_csv() -> str:
    """The README tab: one row per results column with its meaning."""
    return to_csv(("column", "meaning"), [{"column": name, "meaning": DESCRIPTIONS[name]} for name in COLUMNS])


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
    parser.add_argument("--layout-output", type=Path, help="layout.json to write")
    parser.add_argument("--readme-output", type=Path, help="the README tab as CSV")
    args = parser.parse_args()
    summary = json.loads(args.summary.read_text())
    args.output.write_text(to_csv(COLUMNS, rows(summary)))
    if args.readme_output:
        args.readme_output.write_text(readme_csv())
    if args.layout_output:
        args.layout_output.write_text(json.dumps(layout(summary), indent=2, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
