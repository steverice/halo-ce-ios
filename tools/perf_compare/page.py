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


PINNED_REF = "main"  # replace with the merged commit that holds the harness the numbers came from
CODE_URL = f"https://github.com/steverice/visr/blob/{PINNED_REF}/tools/perf_compare"
SITE_PAGE_URL = "https://code.steverice.org/visr/benchmarks.html"


def suite_section(rows: list[dict]) -> list[str]:
    """Per-suite definitions, the overall rule and the rejected-attempt count."""
    rejected = [row for row in rows if row.get("status") == "rejected"]
    reasons = sorted({row["notes"].split(": ", 1)[-1] for row in rejected})
    lines = ["### Suites", "",
             "- **Campaign:** every measured window: the opening of a10, the opening of b30, and the b30 beach encounter 90 "
             "seconds later (`b30 +90 s`). One player, no input, default enemies. Each window warms up for 15 seconds after player readiness, takes a five-second background CPU baseline, then samples for "
             "60 seconds. Three rounds per set, with the frame cap and vsync off and the matched-stock settings for the "
             "headline charts. A set with fewer than three valid rounds is marked incomplete.",
             "- **Multiplayer:** two maps, Chill Out and Blood Gulch, each a window of the same length; the suite value is the "
             "mean over both maps' windows. No measurements yet.",
             "- **Co-op:** local split-screen campaign co-op, two players. Enemy scaling is set explicitly to static 1x on every "
             "port that has the option: VISR sets `debug.enemy_multiplier = 1`, and OpenCE-macOS and CoreCE set their "
             "enemies-per-player scaling to static 1x. A port without the option runs its own fixed behavior, and its notes "
             "column in the spreadsheet says so. A port without split screen shows "
             "\"Does not support split screen\" and the status `not_supported`; that differs from \"not measured\", which "
             "means the suite has not been run for the build.",
             "- **Overall:** per build, the geometric mean of the suites' average rates and the geometric mean of their 1% low "
             "rates, each suite weighted equally, so every suite counts the same whatever its frame-rate scale. A build "
             "missing a suite has no overall bar.", "",
             "A round is rejected when the outside-process CPU average rises more than half a core above the baseline taken "
             "before the window. Each round allows three attempts. Rejected attempts appear in the spreadsheet with the status "
             f"`rejected` and their reason. This snapshot has {len(rejected)} rejected attempts"
             + (f" ({', '.join(reasons)})." if reasons else "."), "",
             f"Suite definitions and the harness are in [`tools/perf_compare`]({CODE_URL}) "
             f"([`adapters.py`]({CODE_URL}/adapters.py), [`refresh.py`]({CODE_URL}/refresh.py)).", ""]
    return lines


def methodology(summary: dict, rows: list[dict]) -> list[str]:
    records = sorted(summary.get("records", []), key=_record_order)
    lines = ["## Methodology", "",
             "Each window's value is the mean over its valid rounds. A bar is the geometric mean of the windows' average "
             "presentation rates, drawn beside the geometric mean of their 1% low rates (1000 divided by the 99th percentile "
             "interval). When a build was measured on several "
             "dates, the newest set is charted. Sets with fewer than three rounds, and targets that could not run, stay in the "
             "spreadsheet with their status. On-time share uses the recorded display rate with a 5% interval tolerance, is "
             "omitted for uncapped rows and does not establish a display tier. The methods measure different events and are "
             "not ranked against one another. Web builds run in a browser that always syncs to the display, so they have "
             "capped numbers only and do not appear in the uncapped charts. The spreadsheet's README tab describes each "
             "column.", ""]
    lines.extend(suite_section(rows))
    lines.extend(_notes(records))
    groups = [group for group in ("trace", "hud", "raf", "other")
              if any(method_group(r) == group and r.get("runs") for r in records)]
    lines.extend(line for group in groups for line in (METHOD_NOTES[group], ""))
    return lines


def render_page(summary: dict, rows: list[dict], specs: list[dict]) -> str:
    """Deterministic page text from the public summary, the results snapshot and the chart specs."""
    if summary and summary.get("schema") != SCHEMA:
        raise ValueError("summary schema mismatch")
    records = sorted(summary.get("records", []), key=_record_order)
    dates = sorted(str(r["date"]) for r in records if r.get("date"))
    lines = ["# Performance benchmarks", "", CAVEAT, "",
             f"Last measured: {dates[-1] if dates else 'unknown'}. The [results spreadsheet]({SHEET_URL}) holds every number. "
             f"The [interactive version]({SITE_PAGE_URL}) adds suites and a choice of builds. The methodology is at the bottom "
             "of this page.", "",
             "## Results", "", "Campaign suite, newest build of each port.", "", charts.BEGIN, "",
             charts.render_all(rows, specs).rstrip(), "", charts.END, ""]
    lines.extend(methodology(summary, rows))
    return "\n".join(lines).rstrip() + "\n"
