"""Build the interactive Pages version of the benchmarks page, with a no-JavaScript default view."""
from __future__ import annotations

from html import escape
import json
from pathlib import Path
import re
import sys

from markdown_it import MarkdownIt

try:
    from . import charts
    from .page import CAVEAT, SHEET_URL, methodology
except ImportError:
    import charts
    from page import CAVEAT, SHEET_URL, methodology

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "docs/site"
MERMAID = "https://cdn.jsdelivr.net/npm/mermaid@11.12.0/dist/mermaid.min.js"
MERMAID_SRI = "sha384-o+g/BxPwhi0C3RK7oQBxQuNimeafQ3GE/ST4iT2BxVI4Wzt60SH4pq9iXVYujjaS"  # sha384 of the pinned file; recompute when the version changes

STYLE = """
.page { max-width: 1160px; }
.bench-bar { display: flex; flex-wrap: wrap; gap: 8px 20px; align-items: center; border: 1px solid var(--line); padding: 10px 14px; margin: 0 0 24px; }
.bench-bar legend { font-weight: 600; padding: 0 6px; }
.bench-bar label { cursor: pointer; }
.bench { margin: 0 0 40px; }
.bench-grid { display: grid; grid-template-columns: minmax(0, 1fr); gap: 16px; align-items: start; }
@media (min-width: 900px) { .bench-grid { grid-template-columns: minmax(0, 1.7fr) minmax(240px, 1fr); } }
.bench-chart { background: #fff; color: #111; border-radius: 8px; padding: 8px; overflow-x: auto; }
.bench-chart svg, .bench-chart img { max-width: 100%; height: auto; display: block; margin: 0 auto; }
.bench-rows { border: 1px solid var(--line); padding: 8px 14px 12px; margin: 0; }
.bench-rows legend { font-weight: 600; padding: 0 6px; }
.bench-rows label { display: block; margin: 6px 0; line-height: 1.35; }
.bench-rows label.off { color: var(--muted); }
.bench-legend, .bench-caption { color: var(--muted); font-size: 0.9rem; margin: 8px 0 0; }
.bench-empty { color: #333; margin: 0; padding: 24px 12px; }
.method { max-width: 760px; margin-inline: auto; }
"""


def slug(spec: dict) -> str:
    return re.sub(r"^\d+-", "", spec["name"])


def spec_payload(specs: list[dict]) -> str:
    keep = ("heading", "paragraph", "empty", "box", "series_labels", "chart", "query", "rename")
    payload = [{"slug": slug(spec), **{key: spec[key] for key in keep if key in spec}} for spec in specs]
    return json.dumps(payload, ensure_ascii=False, indent=1).replace("</", "<\\/")


def section(spec: dict, rows: list[dict], suite: str) -> str:
    built = charts.model(rows, spec["query"], suite)
    name = slug(spec)
    text = charts.source(spec, built, suite)
    if text is None:
        chart = f'<p class="bench-empty">{escape(charts.empty_text(spec, built))}</p>'
    elif (SITE / f"benchmarks/static-{name}.svg").exists():
        chart = (f'<img src="benchmarks/static-{name}.svg" alt="{escape(spec["heading"])}: average and 1% low fps '
                 f'per build, {escape(charts.SUITE_NAMES[suite])} suite">')
    else:
        chart = f"<pre>{escape(text)}</pre>"
    items = []
    for entry in built["entries"]:
        detail = f"build {entry['build'] or 'unknown'}, {entry['date']}"
        if entry["value"] is None:
            word = "Not supported" if entry["kind"] == "not_supported" else "Not measured"
            items.append(f'<label class="off"><input type="checkbox" value="{escape(charts.entry_key(entry))}" disabled> '
                         f'{escape(entry["label"])} ({escape(detail)}). {word}: {escape(entry["reason"])}.</label>')
        else:
            checked = " checked" if entry["default"] else ""
            items.append(f'<label><input type="checkbox" value="{escape(charts.entry_key(entry))}"{checked}> '
                         f'{escape(entry["label"])} ({escape(detail)})</label>')
    rows_html = f'<legend>Builds</legend>{"".join(items)}' if items else "<legend>Builds</legend>"
    legend = charts.legend(spec) if built["entries"] else ""
    return f"""<section class="bench" data-platform="{name}" aria-labelledby="h-{name}">
<h2 id="h-{name}">{escape(spec["heading"])}</h2>
<p>{escape(spec["paragraph"].strip())}</p>
<div class="bench-grid">
<div class="bench-chart">{chart}</div>
<fieldset class="bench-rows">{rows_html}</fieldset>
</div>
<p class="bench-legend">{escape(legend)}</p>
<p class="bench-caption">{escape(charts.caption(built))}</p>
</section>"""


def render_site_page(summary: dict, rows: list[dict], specs: list[dict], legal_html: str) -> str:
    md = MarkdownIt("commonmark", {"html": False})
    suites = "".join(
        f'<label><input type="radio" name="suite" value="{name}"{" checked" if name == charts.DEFAULT_SUITE else ""}> '
        f"{charts.SUITE_NAMES[name]}</label>" for name in charts.SUITE_CHOICES)
    dates = sorted(str(r["date"]) for r in summary.get("records", []) if r.get("date"))
    intro = md.render(f"{CAVEAT}\n\nLast measured: {dates[-1] if dates else 'unknown'}. The "
                      f"[results spreadsheet]({SHEET_URL}) holds every number. The methodology is at the bottom of this page.")
    method = md.render("\n".join(methodology(summary, rows)))
    body = "\n".join(section(spec, rows, charts.DEFAULT_SUITE) for spec in specs)
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>VISR performance benchmarks</title>
<meta name="description" content="Frame-rate comparisons of VISR with other Halo: Combat Evolved ports on Apple devices.">
<meta name="color-scheme" content="light dark">
<link rel="icon" type="image/png" href="icon.png">
<link rel="stylesheet" href="style.css">
<style>{STYLE}</style>
</head>
<body>
<main class="page" id="bench-root" data-csv="benchmarks/results.csv">
<h1>Performance benchmarks</h1>
{intro.rstrip()}
<fieldset class="bench-bar"><legend>Suite</legend>{suites}</fieldset>
{body}
<div class="method">
{method.replace("<h2>Methodology</h2>", "<h2>Methodology</h2>", 1).rstrip()}
</div>
<footer class="foot">
{legal_html}
</footer>
</main>
<script type="application/json" id="chart-specs">{spec_payload(specs)}</script>
<script src="{MERMAID}" integrity="{MERMAID_SRI}" crossorigin="anonymous"></script>
<script src="benchmarks/benchmarks.js"></script>
</body>
</html>
"""
