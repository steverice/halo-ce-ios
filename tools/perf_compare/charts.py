#!/usr/bin/env python3
"""Headline benchmark charts: the entry model, Mermaid source, and the page's default view.

The browser page (`docs/site/benchmarks/benchmarks.js`) implements the same model; `test_perf_compare_sheet.py`
checks that the two produce identical chart text for the committed snapshot.
"""
from __future__ import annotations

import argparse
import csv
import math
import re
from pathlib import Path
import statistics
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[2]
SNAPSHOT = ROOT / "docs/benchmarks/results.csv"
SPECS = ROOT / "docs/benchmarks/charts"
REASONS = ROOT / "docs/benchmarks/reasons.toml"
PAGE = ROOT / "docs/benchmarks.md"
BEGIN = "<!-- BEGIN CHARTS -->"
END = "<!-- END CHARTS -->"

SUITES = ("campaign", "combat", "cutscenes", "multiplayer", "co-op")
SUITE_CHOICES = (*SUITES, "overall")
SUITE_NAMES = {"campaign": "Campaign", "combat": "Combat", "cutscenes": "Cutscenes", "multiplayer": "Multiplayer",
               "co-op": "Co-op", "overall": "Overall"}
DEFAULT_SUITE = "campaign"


def load_snapshot(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def load_specs(directory: Path) -> list[dict]:
    specs = []
    for path in sorted(directory.glob("*.chart.toml")):
        spec = tomllib.loads(path.read_text(encoding="utf-8"))
        spec["name"] = path.name.removesuffix(".chart.toml")
        specs.append(spec)
    return sorted(specs, key=lambda spec: (spec.get("order", 1000), spec["name"]))


def load_reasons(path: Path = REASONS) -> dict:
    return tomllib.loads(path.read_text(encoding="utf-8"))


def plain_reason(reason: str, reasons: dict) -> str:
    """The page's sentence for a reason code; unknown codes and already-plain notes pass through."""
    if reason in reasons["exact"]:
        return reasons["exact"][reason]
    if reason.startswith("no ") and reason.endswith(" measurements yet"):
        return reasons["patterns"]["no_suite_data"].format(suite=reason[3:-len(" measurements yet")])
    match = re.fullmatch(r"not measured in (.+?): (.+)", reason)
    if match:
        return reasons["patterns"]["partial_suite"].format(suite=match[1], windows=match[2])
    if reason.startswith("missing "):
        return reasons["patterns"]["missing_suites"].format(suites=reason[len("missing "):])
    return reason


def _matches(row: dict, filters: dict) -> bool:
    return all(row.get(column, "") in [str(v) for v in (wanted if isinstance(wanted, list) else [wanted])]
               for column, wanted in filters.items())


def _nice_ceiling(value: float) -> float:
    if value <= 0:
        return 1
    magnitude = 10 ** math.floor(math.log10(value))
    for step in (1, 1.5, 2, 2.5, 3, 4, 5, 6, 8, 10):
        if step * magnitude >= value:
            return step * magnitude
    return 10 * magnitude


def _number(value: float) -> str:
    return f"{value:.1f}".removesuffix(".0")


def entry_key(entry: dict) -> str:
    return "@".join((entry["port"], entry["version"], entry["build"]))


# Extra per-build figures a chart spec can list under `query.details`, shown in the caption beside the fps bars:
# column -> (unit, description, how the suite's windows combine). Frame times combine as a geometric mean, shares as a mean.
DETAILS = {"p95_ms": (" ms", "95th-percentile frame time", "geometric", 1),
           "on_time_pct": ("%", "of frames on time", "arithmetic", 1),
           "gpu_p50_ms": (" ms", "median GPU time per rendered frame", "geometric", 2),
           "gpu_p90_ms": (" ms", "90th-percentile GPU time per rendered frame", "geometric", 2)}


def _details(rows: list[dict], query: dict) -> dict:
    """The listed detail columns for the windows, each None unless every window reports it."""
    found = {}
    for column in query.get("details", []):
        values = [row.get(column, "") for row in rows]
        if any(value == "" for value in values):
            found[column] = None
        else:
            numbers = [float(value) for value in values]
            found[column] = (statistics.geometric_mean(numbers) if DETAILS[column][2] == "geometric"
                             else statistics.fmean(numbers))
    return found


def _suite_value(group: list[dict], query: dict, suite: str) -> tuple[dict | None, str, str]:
    """Average and 1% low for one suite on the reference box, or None with the reason and its kind
    (`not_measured`, or `not_supported` when the port lacks what the suite needs)."""
    box = [row for row in group if row["reference_box"].lower() == "true"]
    unsupported = [row for row in group if row["suite"] == suite and row["status"] == "not_supported"]
    if unsupported:
        return None, unsupported[0]["notes"] or "not supported", "not_supported"
    if not box:
        gone = [row["notes"] for row in group if row["status"] == "unavailable" and row["notes"]]
        return None, gone[0] if gone else "not measured on the reference box", "not_measured"
    mine = [row for row in box if row["suite"] == suite]
    if not mine:
        return None, f"no {suite} measurements yet", "not_measured"
    usable = {}
    for row in sorted((r for r in mine if _matches(r, query["filter"])
                       and r.get(query["metrics"][0], "") != ""), key=lambda r: r["date"]):
        usable[row["window"]] = row
    if not usable:
        skipped = [row for row in mine if row["status"] == "not_measured" and row["notes"]]
        if skipped:
            return None, skipped[0]["notes"], "not_measured"
        return None, query.get("no_data_reason", "no matching measurement"), "not_measured"
    required = query.get("suite_windows", {}).get(suite, [])
    missing = [name for name in required if name not in usable]
    if missing:
        # A suite bar covers the same windows for every port: a build with only some of them is listed, not drawn.
        return None, f"not measured in {SUITE_NAMES[suite]}: {', '.join(missing)}", "not_measured"
    rows = list(usable.values())
    return {"avg": statistics.geometric_mean(float(r["fps_avg"]) for r in rows),
            "low": (statistics.geometric_mean(float(r["fps_1pct_low"]) for r in rows)
                    if "fps_1pct_low" in query["metrics"] and all(r.get("fps_1pct_low", "") != "" for r in rows) else None),
            "details": _details(rows, query), "windows": sorted(usable), "rows": rows}, "", ""


def model(rows: list[dict], query: dict, suite: str) -> dict:
    """Every build of the platform's ports as an entry, with its values for the suite or the reason it has none."""
    platform_rows = [row for row in rows if row["platform"] == query["platform"]]
    groups: dict[tuple, list[dict]] = {}
    for row in platform_rows:
        groups.setdefault((row["port"], row["version"], row["build"]), []).append(row)
    entries = []
    for (port, version, build), group in groups.items():
        base = {"port": port, "port_name": group[0]["port_name"], "version": version, "build": build,
                "label": group[0]["label"], "date": max(row["date"] for row in group),
                "url": next((row.get("release_url", "") for row in group if row.get("release_url", "")), ""),
                "gpu_note": query.get("no_gpu_notes", {}).get(port, "")}
        if suite == "overall":
            # Which suites count is decided per chart, from the builds drawn (see `_drawn`); here each build keeps
            # its own suite values. A build with none is listed with the first suite's reason.
            parts = {name: _suite_value(group, query, name) for name in SUITES}
            found = {name: part[0] for name, part in parts.items() if part[0] is not None}
            if found:
                value, reason, kind = {"avg": 0.0, "low": None, "details": {}, "rows": []}, "", ""
            else:
                first = parts[SUITES[0]]
                value, reason = None, first[1]
                kind = "not_supported" if {part[2] for part in parts.values()} == {"not_supported"} else "not_measured"
            base["suites"] = found
        else:
            value, reason, kind = _suite_value(group, query, suite)
        entries.append({**base, "value": value, "reason": reason, "kind": kind})
    order = query.get("port_order", [])
    entries.sort(key=lambda e: (order.index(e["port"]) if e["port"] in order else len(order), e["port"],
                                tuple(-ord(c) for c in e["date"]), e["version"], e["build"]))
    newest: dict[str, dict] = {}
    for entry in entries:
        if entry["value"] is not None and (entry["port"] not in newest or entry["date"] > newest[entry["port"]]["date"]):
            newest[entry["port"]] = entry
    for entry in entries:
        entry["default"] = newest.get(entry["port"]) is entry
    return {"overall": suite == "overall", "entries": entries, "box": next((r["host"] for r in platform_rows if r["reference_box"].lower() == "true"), "")}


def overall_suites(pool: list[dict]) -> list[str]:
    """The suites Overall covers on a chart: every suite at least one drawn build has complete. A build missing one
    of them is left out of Overall (see `overall_split`), so one incomplete port does not shrink the set for the rest."""
    return [name for name in SUITES if any(e["suites"].get(name) for e in pool)]


def overall_split(pool: list[dict]) -> tuple[list[str], list[dict], list[dict]]:
    """(suites, builds drawn, builds left out) for Overall."""
    suites = overall_suites(pool)
    kept = [e for e in pool if suites and all(e["suites"].get(name) for name in suites)]
    return suites, kept, [e for e in pool if e not in kept]


def _overall_value(entry: dict, covered: list[str]) -> dict:
    parts = [entry["suites"][name] for name in covered]
    details = {}
    for column in {c for p in parts for c in (p.get("details") or {})}:
        values = [(p.get("details") or {}).get(column) for p in parts]
        details[column] = None if any(v is None for v in values) else (
            statistics.geometric_mean(values) if DETAILS[column][2] == "geometric" else statistics.fmean(values))
    return {"avg": statistics.geometric_mean(p["avg"] for p in parts), "details": details,
            "low": (statistics.geometric_mean(p["low"] for p in parts) if all(p["low"] is not None for p in parts) else None),
            "rows": [r for p in parts for r in p["rows"]]}


def _pool(built: dict, selected: set[str] | None) -> list[dict]:
    return [e for e in built["entries"] if e["value"] is not None
            and (entry_key(e) in selected if selected is not None else e["default"])]


def _drawn(built: dict, selected: set[str] | None) -> list[dict]:
    """The builds a chart draws. For Overall, the geometric mean runs over the suites above, for the builds that
    have all of them."""
    pool = _pool(built, selected)
    if not built.get("overall"):
        return pool
    suites, kept, _ = overall_split(pool)
    return [{**e, "value": _overall_value(e, suites)} for e in kept]


def _names(items: list[str]) -> str:
    return items[0] if len(items) == 1 else ", ".join(items[:-1]) + " and " + items[-1]


def overall_note(built: dict, selected: set[str] | None = None) -> str:
    """The caption sentences naming the suites Overall covers and the ticked builds it leaves out, or "" for any
    other suite."""
    if not built.get("overall"):
        return ""
    suites, kept, left = overall_split(_pool(built, selected))
    if not kept:
        return ""
    note = "Overall covers " + _names([SUITE_NAMES[name] for name in suites]) + "."
    for e in left:
        gaps = [SUITE_NAMES[name] for name in suites if not e["suites"].get(name)]
        note += f" {e['label']} is not in Overall: {_names(gaps)} not measured completely."
    return note


def chart_title(spec: dict, built: dict, suite: str) -> str:
    box = built["box"] or spec.get("box", "")
    unit = spec.get("chart", {}).get("title_unit", "uncapped fps")
    return f"{SUITE_NAMES[suite]}: {unit}, {box}".replace('"', "'")


def header_text(chart: dict) -> str:
    """The hand-written Mermaid frontmatter with the light-mode bar colors filled in."""
    return chart.get("header", "").strip().replace("__PALETTE__", ", ".join(chart.get("palette", [])))


def source(spec: dict, built: dict, suite: str, selected: set[str] | None = None) -> str | None:
    """Mermaid text for the ticked entries that have values, or None when there is nothing to draw."""
    drawn = _drawn(built, selected)
    if not drawn:
        return None
    chart, rename = spec["chart"], spec.get("rename", {})
    labels = [(f"{rename.get(e['port_name'], e['port_name'])} {e['version']}"
               + (" (pre-release)" if e["label"].endswith("(pre-release)") else "")).replace('"', "'") for e in drawn]
    averages = [e["value"]["avg"] for e in drawn]
    # A build without a 1% low draws no bar in the second series (a zero-length bar) instead of dropping the series.
    known = [e["value"]["low"] for e in drawn if e["value"]["low"] is not None]
    # The 1% low never exceeds the average, so a low that rounds above it (a capped run) is drawn at the average and
    # does not cover the average bar.
    lows = [min(e["value"]["low"], e["value"]["avg"]) if e["value"]["low"] is not None else 0 for e in drawn]
    with_lows = bool(known)
    y_max = chart.get("y_max", _nice_ceiling(max(averages + known) * 1.25))
    head = "xychart-beta horizontal" if chart.get("horizontal") else "xychart-beta"
    quoted = ", ".join('"' + label + '"' for label in labels)
    lines = [header_text(chart), head, f'    title "{chart_title(spec, built, suite)}"',
             f'    x-axis "{chart.get("x_label", "")}" [{quoted}]',
             f'    y-axis "{chart.get("y_label", "")}" {_number(chart.get("y_min", 0))} --> {_number(y_max)}',
             f"    bar [{', '.join(_number(v) for v in averages)}]",
             f"    bar [{', '.join(_number(v) for v in lows)}]" if with_lows else ""]
    return "\n".join(line for line in lines if line)


def _lower_is_better(column: str) -> bool:
    return column.endswith("_ms") or column.startswith(("gpu_", "cpu_", "frame_cost"))


def direction_note(spec: dict, built: dict | None = None) -> str:
    """"Higher is better." or "Lower is better." from the charted metrics; a chart that mixes directions says which
    series each applies to. With `built`, a chart that draws no 1% low series leaves it out."""
    columns = list(spec["query"]["metrics"])
    labels = spec["series_labels"]
    if built is not None and not has_lows(built):
        columns, labels = columns[:1], labels[:1]
    lower = [_lower_is_better(column) for column in columns]
    if all(lower):
        return "Lower is better."
    if not any(lower):
        return "Higher is better."
    higher_names = ", ".join(label for label, flag in zip(labels, lower) if not flag)
    lower_names = ", ".join(label for label, flag in zip(labels, lower) if flag)
    return f"Higher is better for {higher_names}; lower is better for {lower_names}."


def has_lows(built: dict, selected: set[str] | None = None) -> bool:
    """Whether any drawn build has a 1% low, so the second bar series is drawn."""
    drawn = _drawn(built, selected)
    return any(e["value"]["low"] is not None for e in drawn)


def legend(spec: dict, built: dict | None = None) -> str:
    marks = spec["chart"].get("marks", [])
    names = spec["series_labels"] if built is None or has_lows(built) else spec["series_labels"][:1]
    return " ".join(f"{marks[i % len(marks)]} {name}" for i, name in enumerate(names))


def _detail_text(entry: dict, columns: list[str]) -> str:
    parts = []
    for column in columns:
        value = (entry["value"].get("details") or {}).get(column)
        if value is not None:
            unit, description, _, places = DETAILS[column]
            text = f"{value:.{places}f}"
            text = text.rstrip("0").rstrip(".") if "." in text else text
            parts.append(f"{text} ms {description}" if unit == " ms" else f"{text}% {description}")
    if parts:
        return ", ".join(parts)
    if entry.get("gpu_note") and all(column.startswith("gpu_") for column in columns):
        return entry["gpu_note"]
    what = ("on-time share" if columns == ["on_time_pct"] else "GPU times"
            if all(column.startswith("gpu_") for column in columns) else "frame times")
    return f"no {what} reported"


def _missing_windows(entry: dict, windows: list[str]) -> str:
    """"; b30 not measured yet" for the windows a chart expects and a build has no result for."""
    have = entry["value"].get("windows")
    missing = [name for name in windows if have is not None and name not in have]
    return f"; {', '.join(missing)} not measured yet" if missing else ""


def caption_parts(built: dict, selected: set[str] | None = None, columns: list[str] | None = None,
                  windows: list[str] | None = None) -> list[tuple[str, str]]:
    """The caption as (text, url) pieces; a piece with a url is a build's label, linked to its release page."""
    drawn = _drawn(built, selected)
    if not drawn:
        return []
    columns = columns or []
    # With no windows listed in the spec, a build that lacks a window another drawn build has is flagged.
    windows = windows or sorted({name for e in drawn for name in (e["value"].get("windows") or [])})
    note = overall_note(built, selected)
    parts = [(f"{built['box']}. " + (note + " " if note else ""), "")]
    for index, e in enumerate(drawn):
        parts.append((e["label"], e.get("url", "")))
        parts.append((f" (build {e['build'] or 'unknown'}, measured {e['date']}"
                      + (f"; {_detail_text(e, columns)}" if columns else "") + _missing_windows(e, windows) + ")"
                      + ("; " if index < len(drawn) - 1 else "."), ""))
    return parts


def caption(built: dict, selected: set[str] | None = None, columns: list[str] | None = None, links: str = "",
            windows: list[str] | None = None) -> str:
    """The caption text; `links` is "markdown" or "" (plain)."""
    return "".join(f"[{text}]({url})" if url and links == "markdown" else text
                   for text, url in caption_parts(built, selected, columns, windows))


def empty_text(spec: dict, built: dict) -> str:
    """The explanation for a platform with no builds, or for a selection with nothing to draw."""
    if not built["entries"]:
        return spec["empty"].strip()
    if built.get("overall") and any(e["value"] is not None for e in built["entries"]):
        return "No build in this chart has every suite Overall covers."
    return "No build has data for this suite yet."


def _link(entry: dict) -> str:
    return f"[{entry['label']}]({entry['url']})" if entry.get("url") else entry["label"]


def render_section(spec: dict, rows: list[dict], suite: str = DEFAULT_SUITE, reasons: dict | None = None) -> str:
    """The Markdown for one platform's default view: the chart if any build has data, else why not."""
    reasons = reasons or load_reasons()
    built = model(rows, spec["query"], suite)
    parts = [f"### {spec['heading']}", "", spec["paragraph"].strip(), ""]
    text = source(spec, built, suite)
    if text is None:
        parts.extend([empty_text(spec, built), ""])
    else:
        parts.extend(["```mermaid", text, "```", "", legend(spec, built), "", direction_note(spec, built), "", caption(built, None, spec["query"].get("details"), "markdown", spec["query"].get("windows")), ""])
    for kind, heading in (("not_supported", "Not supported"), ("not_measured", "Not measured")):
        skipped = [e for e in built["entries"] if e["value"] is None and e["kind"] == kind]
        if skipped:
            parts.extend([heading + ": " + "; ".join(f"{_link(e)} ({plain_reason(e['reason'], reasons)})" for e in skipped) + ".", ""])
    return "\n".join(parts)


def render_all(rows: list[dict], specs: list[dict], suite: str = DEFAULT_SUITE, reasons: dict | None = None) -> str:
    reasons = reasons or load_reasons()
    return "\n".join(render_section(spec, rows, suite, reasons) for spec in specs).rstrip() + "\n"


def apply(page: str, block: str) -> str:
    """Replace the text between the chart markers."""
    start, end = page.find(BEGIN), page.find(END)
    if start < 0 or end < start:
        raise ValueError("the page has no chart markers")
    return page[:start + len(BEGIN)] + "\n\n" + block.rstrip() + "\n\n" + page[end:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, default=SNAPSHOT)
    parser.add_argument("--specs", type=Path, default=SPECS)
    parser.add_argument("--page", type=Path, default=PAGE, help="page with chart markers to update")
    parser.add_argument("--suite", default=DEFAULT_SUITE, choices=SUITE_CHOICES)
    parser.add_argument("--check", action="store_true", help="fail when the page's charts are stale")
    parser.add_argument("--stdout", action="store_true", help="print the chart section instead of editing the page")
    args = parser.parse_args()
    block = render_all(load_snapshot(args.snapshot), load_specs(args.specs), args.suite)
    if args.stdout:
        sys.stdout.write(block)
        return 0
    page = args.page.read_text(encoding="utf-8")
    updated = apply(page, block)
    if args.check:
        if updated != page:
            print(f"stale charts: {args.page.name}", file=sys.stderr)
            return 1
        return 0
    args.page.write_text(updated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
