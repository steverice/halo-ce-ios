#!/usr/bin/env python3
"""Generate Mermaid xychart blocks from the results snapshot and the chart specs."""
from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[2]
SNAPSHOT = ROOT / "docs/benchmarks/results.csv"
SPECS = ROOT / "docs/benchmarks/charts"
PAGE = ROOT / "docs/benchmarks.md"
BEGIN = "<!-- BEGIN CHARTS -->"
END = "<!-- END CHARTS -->"


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


def _matches(row: dict, filters: dict) -> bool:
    for column, wanted in filters.items():
        values = wanted if isinstance(wanted, list) else [wanted]
        if row.get(column, "") not in [str(value) for value in values]:
            return False
    return True


def select(rows: list[dict], query: dict) -> list[dict]:
    """Rows that match the query's filters, one per x and series, newest measurement first."""
    chosen = [row for row in rows if _matches(row, query.get("filter", {}))
              and not _matches_any(row, query.get("exclude", {}))
              and row.get(query["metric"], "") != ""]
    latest: dict[tuple, dict] = {}
    for row in sorted(chosen, key=lambda row: row.get("date", "")):
        key = (tuple(row.get(column, "") for column in query["x"]), row.get(query.get("series", ""), ""))
        latest[key] = row
    return list(latest.values())


def _matches_any(row: dict, excluded: dict) -> bool:
    return bool(excluded) and any(
        row.get(column, "") in [str(value) for value in (wanted if isinstance(wanted, list) else [wanted])]
        for column, wanted in excluded.items())


def _ordered(values: list[str], preferred: list[str] | None) -> list[str]:
    seen = list(dict.fromkeys(values))
    if not preferred:
        return sorted(seen)
    return [value for value in preferred if value in seen] + sorted(value for value in seen if value not in preferred)


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


def _label(text: str, rename: dict) -> str:
    return str(rename.get(text, text)).replace('"', "'")


def _rank(value: str, preferred: list[str] | None) -> tuple:
    return (preferred.index(value), "") if preferred and value in preferred else (len(preferred or []), value)


def _build_metrics(spec: dict, rows: list[dict]) -> dict:
    """One category per row (the x columns joined) and one series per metric column, drawn in order."""
    query, chart = spec["query"], spec["chart"]
    rename = spec.get("rename", {})
    chosen = select(rows, {**query, "metric": query["metrics"][0]})
    chosen = [row for row in chosen if all(row.get(metric, "") != "" for metric in query["metrics"])]
    if not chosen:
        raise ValueError(f"chart {spec['name']}: no rows match the query")
    order = query.get("order", {})
    chosen.sort(key=lambda row: tuple(_rank(row[column], order.get(column)) for column in query["x"]))
    labels = [" ".join(_label(row[column], rename) for column in query["x"]) for row in chosen]
    data = [[float(row[metric]) for row in chosen] for metric in query["metrics"]]
    y_min = chart.get("y_min", 0)
    y_max = chart.get("y_max", _nice_ceiling(max(max(line) for line in data) * 1.08))
    title = chart["title"].replace('"', "'")
    head = "xychart-beta horizontal" if chart.get("horizontal") else "xychart-beta"
    lines = [chart.get("header", "").strip(), head, f'    title "{title}"',
             f'    x-axis "{chart.get("x_label", "")}" [{", ".join(chr(34) + label + chr(34) for label in labels)}]',
             f'    y-axis "{chart.get("y_label", "")}" {_number(y_min)} --> {_number(y_max)}']
    lines.extend(f"    bar [{', '.join(_number(value) for value in line)}]" for line in data)
    marks = chart.get("marks", [])
    names = spec["series_labels"]
    legend = " ".join(f"{marks[i % len(marks)]} {name}" for i, name in enumerate(names))
    seen = {}
    for row in chosen:
        seen.setdefault(row["label"], row)
    builds = "; ".join(f"{label} (build {row['build'] or 'unknown'}"
                       + (f", source `{row['source']}`" if row["source"] else "") + ")" for label, row in seen.items())
    host = ", ".join(sorted({row["host"] for row in chosen}))
    dates = ", ".join(sorted({row["date"] for row in chosen}))
    return {"source": "\n".join(line for line in lines if line), "legend": legend,
            "caption": f"{host}. {builds}. Measured {dates}."}


def build(spec: dict, rows: list[dict]) -> dict:
    """The chart's Mermaid source, legend and caption, as plain strings."""
    if "metrics" in spec["query"]:
        return _build_metrics(spec, rows)
    query, chart = spec["query"], spec["chart"]
    rename = spec.get("rename", {})
    chosen = select(rows, query)
    if not chosen:
        raise ValueError(f"chart {spec['name']}: no rows match the query")
    xs = _ordered([" ".join(row[column] for column in query["x"]) for row in chosen], query.get("order_x"))
    series_column = query.get("series")
    names = _ordered([row[series_column] for row in chosen], query.get("order_series")) if series_column else [""]
    kind = chart.get("type", "bar")
    grouped = bool(series_column) and kind == "bar"
    if grouped:
        # Mermaid overlaps bar series at one x position, so each series gets its own positions.
        categories = [(x, name) for x in _ordered([" ".join(row[c] for c in query["x"]) for row in chosen],
                                                  query.get("order_x")) for name in names]
        labels = [f"{_label(x, rename)} {_label(name, rename)}" for x, name in categories]
    else:
        categories = [(x, "") for x in xs]
        labels = [_label(x, rename) for x, _ in categories]
    values: dict[tuple, float] = {}
    for row in chosen:
        x = " ".join(row[column] for column in query["x"])
        values[(x, row[series_column] if series_column else "")] = float(row[query["metric"]])
    data = []
    for name in names:
        if grouped:
            data.append([values.get((x, n), 0.0) if n == name else 0.0 for x, n in categories])
        else:
            line = [values.get((x, name)) for x, _ in categories]
            if None in line:
                raise ValueError(f"chart {spec['name']}: series {name!r} is missing an x value")
            data.append(line)
    top = max(max(line) for line in data)
    y_min = chart.get("y_min", 0)
    y_max = chart.get("y_max", _nice_ceiling(top * 1.08))
    title = chart["title"].replace('"', "'")
    lines = [chart.get("header", "").strip(), "xychart-beta", f'    title "{title}"',
             f'    x-axis "{chart.get("x_label", "")}" [{", ".join(chr(34) + label + chr(34) for label in labels)}]',
             f'    y-axis "{chart.get("y_label", "")}" {_number(y_min)} --> {_number(y_max)}']
    lines.extend(f"    {kind} [{', '.join(_number(value) for value in line)}]" for line in data)
    source = "\n".join(line for line in lines if line)
    marks = chart.get("marks", [])
    legend = ""
    if series_column and marks:
        legend = " ".join(f"{marks[i % len(marks)]} {name}" for i, name in enumerate(names))
    seen = {}
    for name in names:
        for row in chosen:
            if not series_column or row[series_column] == name:
                seen.setdefault(row["label"], row)
    dates = sorted({row["date"] for row in chosen})
    builds = "; ".join(
        f"{label} (build {row['build'] or 'unknown'}" + (f", source `{row['source']}`" if row["source"] else "") + ")"
        for label, row in seen.items())
    caption = f"{builds}. Measured {', '.join(dates)}." if builds else ""
    return {"source": source, "legend": legend, "caption": caption}


def render_section(spec: dict, rows: list[dict]) -> str:
    parts = [f"### {spec['heading']}", "", spec["paragraph"].strip(), ""]
    try:
        built = build(spec, rows)
    except ValueError:
        # A platform without results shows its explanation, never an empty or invented chart.
        if "empty" not in spec:
            raise
        return "\n".join([*parts, spec["empty"].strip(), ""])
    parts.extend(["```mermaid", built["source"], "```", ""])
    if built["legend"]:
        parts.extend([built["legend"], ""])
    if built["caption"]:
        parts.extend([built["caption"], ""])
    return "\n".join(parts)


def render_all(rows: list[dict], specs: list[dict]) -> str:
    return "\n".join(render_section(spec, rows) for spec in specs).rstrip() + "\n"


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
    parser.add_argument("--check", action="store_true", help="fail when the page's charts are stale")
    parser.add_argument("--stdout", action="store_true", help="print the chart section instead of editing the page")
    args = parser.parse_args()
    block = render_all(load_snapshot(args.snapshot), load_specs(args.specs))
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
