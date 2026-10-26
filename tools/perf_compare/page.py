"""Compose docs/benchmarks.md: headline charts from the results snapshot, then the methodology."""
from __future__ import annotations

try:
    from . import charts
    from .results import CAVEAT, METHOD_NOTES, SCHEMA, _notes, method_group, _record_order
    from .sheet_fetch import SHEET_ID
except ImportError:
    import charts
    from results import CAVEAT, METHOD_NOTES, SCHEMA, _notes, method_group, _record_order
    from sheet_fetch import SHEET_ID

SHEET_URL = f"https://docs.google.com/spreadsheets/d/{SHEET_ID}"


def render_page(summary: dict, rows: list[dict], specs: list[dict]) -> str:
    """Deterministic page text from the public summary, the results snapshot and the chart specs."""
    if summary and summary.get("schema") != SCHEMA:
        raise ValueError("summary schema mismatch")
    records = sorted(summary.get("records", []), key=_record_order)
    dates = sorted(str(r["date"]) for r in records if r.get("date"))
    lines = ["# Performance benchmarks", "", CAVEAT, "",
             f"Last measured: {dates[-1] if dates else 'unknown'}. The [results spreadsheet]({SHEET_URL}) holds every number, "
             "and the methodology is at the bottom of this page.", "",
             "## Results", "", charts.BEGIN, "", charts.render_all(rows, specs).rstrip(), "", charts.END, "",
             "## Methodology", "",
             "Each bar is the mean over three rounds of one window's average presentation rate or interval; the spreadsheet "
             "lists the minimum and maximum of each. When a port was measured on several dates, the newest set is charted. "
             "Sets with fewer than three rounds, and targets that could not run, stay in the spreadsheet with their status. "
             "On-time share uses the recorded display rate with a 5% interval tolerance, is omitted for uncapped rows and "
             "does not establish a display tier. The methods measure different events and are not ranked against one another. "
             "The spreadsheet's README tab describes each column.", ""]
    lines.extend(_notes(records))
    groups = [group for group in ("trace", "hud", "raf", "other")
              if any(method_group(r) == group and r.get("runs") for r in records)]
    lines.extend(line for group in groups for line in (METHOD_NOTES[group], ""))
    if summary.get("sweep"):
        lines.extend(["The spreadsheet's device-sweep tab lists VISR's offscreen throughput reports from iPhones, iPads and "
                      "Apple TVs. Offscreen throughput is a different measurement from displayed pacing and does not "
                      "compare with the charts.", ""])
    lines.extend(["The tool that runs these comparisons is documented in "
                  "[`tools/perf_compare/README.md`](../tools/perf_compare/README.md).", ""])
    return "\n".join(lines).rstrip() + "\n"
