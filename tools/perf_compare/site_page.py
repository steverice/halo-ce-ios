"""Build the interactive Pages version of the benchmarks page, with a no-JavaScript default view."""
from __future__ import annotations

from html import escape
import json
from pathlib import Path
import re
import sys
from string import Template

from markdown_it import MarkdownIt

try:
    from . import charts
    from .page import CAVEAT, SHEET_URL, last_measured, methodology
except ImportError:
    import charts
    from page import CAVEAT, SHEET_URL, last_measured, methodology

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / "docs/site"
MERMAID = "https://cdn.jsdelivr.net/npm/mermaid@11.12.0/dist/mermaid.min.js"
MERMAID_SRI = "sha384-o+g/BxPwhi0C3RK7oQBxQuNimeafQ3GE/ST4iT2BxVI4Wzt60SH4pq9iXVYujjaS"  # sha384 of the pinned file; recompute when the version changes

TITLE = "VISR performance benchmarks"
DESCRIPTION = "Frame-rate comparisons of VISR with other Halo: Combat Evolved ports on Apple devices."


def head_tags(metadata: dict[str, str]) -> dict[str, str]:
    """The icon and social-card tags shared with the other site pages (tools/site_head_*.html, read by build_site.py)."""
    icons = (ROOT / "tools/site_head_icons.html").read_text(encoding="utf-8").rstrip("\n")
    social = Template((ROOT / "tools/site_head_social.html").read_text(encoding="utf-8")).substitute(
        {key: escape(value, quote=True) for key, value in metadata.items()}).rstrip("\n")
    return {"head_icons": icons, "head_social": social}


STYLE = """
.page { max-width: 1160px; }
.page > h1 { margin: 12px 0 14px; }
.page > p { max-width: 70ch; color: var(--muted); font-size: 0.95rem; line-height: 1.55; margin-bottom: 10px; }
.page > p:first-of-type { color: var(--ink); }
.sr-only { position: absolute; width: 1px; height: 1px; margin: -1px; padding: 0; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; border: 0; }

/* Controls: the suite switch is a row of pills; the radios stay in the tab order behind them. */
.bench-bar { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; min-width: 0; border: 0;
  padding: 10px 0; margin: 18px 0 8px; position: sticky; top: 0; z-index: 20;
  /* Full-width and opaque: the page color spreads past the column on both sides, clipped to the bar's own height. */
  background: var(--bg); box-shadow: 0 0 0 100vmax var(--bg), 0 1px 0 100vmax var(--line); clip-path: inset(0 -100vmax -1px); }
.bench-bar legend { float: left; font-size: 0.8rem; font-weight: 600; letter-spacing: 0.06em; text-transform: uppercase; color: var(--muted); padding: 0; margin-right: 10px; line-height: 2.2; }
.bench-bar label { position: relative; display: inline-flex; align-items: center; min-height: 36px; padding: 0 14px; border: 1px solid var(--line);
  border-radius: 999px; font-size: 0.9rem; cursor: pointer; user-select: none; }
.bench-bar label:hover { border-color: var(--accent); }
.bench-bar input { position: absolute; opacity: 0; inset: 0; margin: 0; cursor: pointer; }
.bench-bar label:has(input:checked) { background: var(--accent); border-color: var(--accent); color: var(--bg); font-weight: 600; }
.bench-bar label:has(input:focus-visible) { outline: 2px solid var(--accent); outline-offset: 2px; }
@media (max-width: 600px) { .bench-bar { position: static; } .bench-bar legend { float: none; width: 100%; line-height: 1.4; } }

.bench { margin: 0 0 24px; padding-top: 36px; margin-top: 36px; }
.bench > h2 { font-size: 1.5rem; margin-bottom: 8px; }
.bench > p { max-width: 75ch; color: var(--muted); font-size: 0.95rem; line-height: 1.55; }
.bench-grid { display: grid; grid-template-columns: minmax(0, 1fr); gap: 20px 32px; align-items: start; margin-top: 18px; }
@media (min-width: 900px) { .bench-grid { grid-template-columns: minmax(0, 1.7fr) minmax(260px, 1fr); } }
/* The device frames come from style.css; only the screen's content box is defined here. */
.bench-media { min-width: 0; }
.device .bench-chart { display: flex; align-items: center; justify-content: center; width: 100%; overflow: hidden;
  background: #fff; color: #111; border-radius: 18px; }
.device-iphone .bench-chart { aspect-ratio: 19.5 / 9; border-radius: 24px; }
.device-ipad .bench-chart { aspect-ratio: 4 / 3; border-radius: 12px; }
.device-tv .bench-chart { aspect-ratio: 16 / 9; border-radius: 3px; }
.device-vision .bench-chart { aspect-ratio: 16 / 9; border-radius: 14px; }
.device-mac .bench-chart { aspect-ratio: 16 / 10; border-radius: 5px; }
.device-vision.device-glass .bench-chart { position: absolute; inset: 0; aspect-ratio: auto; border-radius: 0; box-sizing: border-box; }
.device-glass .bench-chart { padding: 9.3% 12% 13.4%; }
.device-glass .bench-chart svg { width: 100%; height: 100%; }
.device-glass .bench-empty { padding: 4px 10px; text-align: center; font-size: clamp(0.7rem, 2.6vw, 1rem); line-height: 1.3; }
.bench-chart svg, .bench-chart img { display: block; flex: 0 1 auto; min-width: 0; width: 100%; max-width: 100%; height: auto; }
.bench-chart:has(img) { background: #fff; }
@media (prefers-color-scheme: dark) { .device .bench-chart { background: #10232b; color: #e8f6fa; } }
.bench-empty { color: inherit; margin: 0; padding: 24px 12px; font-size: 0.95rem; }
.bench-legend, .bench-direction, .bench-caption { margin: 12px 0 0; font-size: 0.85rem; line-height: 1.5; color: var(--muted); overflow-wrap: anywhere; }
.bench-legend { color: var(--ink); font-size: 0.9rem; }
.swatch { display: inline-block; width: 0.9em; height: 0.9em; border-radius: 2px; margin: 0 0.2em 0 0.6em; vertical-align: -0.1em; }
.swatch:first-child { margin-left: 0; }
.swatch.avg { background: var(--chart-avg); }
.swatch.low { background: var(--chart-low); }

/* Builds: one row per port, its other versions folded under it. */
.bench-rows { min-width: 0; margin: 0; padding: 14px 16px 10px; border: 1px solid var(--line); border-radius: 12px; }
.bench-rows legend { font-size: 0.8rem; font-weight: 600; letter-spacing: 0.06em; text-transform: uppercase; color: var(--muted); padding: 0 6px; }
.build-group { padding: 8px 0; border-top: 1px solid var(--line); }
.build-group:first-of-type { border-top: 0; }
.build { padding: 4px 0; line-height: 1.35; }
.build-label { display: flex; flex-wrap: wrap; align-items: baseline; column-gap: 8px; cursor: pointer; }
.build-label input { flex: none; align-self: center; width: 18px; height: 18px; margin: 0; accent-color: var(--accent); }
.build-name { font-weight: 600; min-width: 0; overflow-wrap: anywhere; }
.build-meta { flex: 1 1 100%; margin-left: 26px; font-size: 0.8rem; font-weight: 400; color: var(--muted); }
.build.off { color: var(--muted); }
.build.off .build-label { cursor: default; }
.build.off .build-name { font-weight: 500; }
.build.off input { opacity: 0.5; }
.why { margin: 2px 0 0 26px; font-size: 0.8rem; }
.why summary.tag { display: inline-block; cursor: pointer; padding: 0 10px; border: 1px solid var(--line); border-radius: 999px; line-height: 1.7; color: var(--muted); list-style: none; }
.why summary.tag::-webkit-details-marker { display: none; }
.why summary.tag::after { content: " \\25BE"; }
.why[open] summary.tag::after { content: " \\25B4"; }
.why summary.tag:hover { border-color: var(--accent); color: var(--ink); }
.why p { margin: 6px 0 4px; line-height: 1.45; color: var(--muted); }
.older { margin: 2px 0 0 26px; }
.older summary { cursor: pointer; font-size: 0.85rem; color: var(--accent); padding: 4px 0; min-height: 28px; }
.older[open] summary { margin-bottom: 2px; }
.older .build { padding-left: 0; }

/* Per-build figures under each chart. On a phone each row becomes a small card. */
.bench-caption { overflow-x: visible; }
.bench-table { width: 100%; border-collapse: collapse; font-size: 0.85rem; line-height: 1.4; margin-top: 12px; }
.bench-table caption { text-align: left; color: var(--muted); padding-bottom: 8px; caption-side: top; }
.bench-table th, .bench-table td { text-align: left; vertical-align: top; padding: 7px 10px 7px 0; border-top: 1px solid var(--line); font-weight: 400; }
.bench-table thead th { border-top: 0; border-bottom: 1px solid var(--line); color: var(--muted); font-size: 0.78rem; font-weight: 600; }
.bench-table tbody th { font-weight: 600; }
.bench-table td { font-variant-numeric: tabular-nums; color: var(--muted); }
.bench-table .build-meta { display: block; margin: 0; overflow-wrap: anywhere; }
.bench-table tbody th { min-width: 9.5em; }
.bench-table td[data-label="Measured"] { white-space: nowrap; }
@media (max-width: 599px) {
  .bench-table, .bench-table tbody, .bench-table tr, .bench-table th, .bench-table td, .bench-table caption { display: block; width: 100%; }
  .bench-table thead { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); }
  .bench-table tr { border-top: 1px solid var(--line); padding: 8px 0; }
  .bench-table th, .bench-table td { border: 0; padding: 1px 0; }
  .bench-table td:empty { display: none; }
  .bench-table td::before { content: attr(data-label) ": "; color: var(--ink); font-weight: 600; }
}

/* The suite bar is sticky, so jump targets keep clear of it. */
h2[id], .bench, .bench-media { scroll-margin-top: 76px; }
@media (max-width: 600px) { h2[id], .bench, .bench-media { scroll-margin-top: 12px; } }

.method { max-width: 760px; margin-inline: auto; margin-top: 56px; padding-top: 28px; border-top: 1px solid var(--line); }
.method h2 { font-size: 1.5rem; }
.method h3 { font-size: 1.05rem; margin: 28px 0 10px; }
.method p, .method li { font-size: 0.95rem; line-height: 1.6; }
.method li { margin-bottom: 10px; }
.method code { white-space: normal; overflow-wrap: anywhere; }
"""


FRAMES = {"mac": "device-mac", "iphone-ipad": "device-ipad", "apple-tv": "device-tv", "vision-pro": "device-vision device-glass"}
# The Vision Pro nav icon's outline with a shallower nose notch: the notch's rise above the lobes is halved
# (the icon's `c-1.6 0-2.7-.9-3.5-2.1`, `c-.6-.8-1.8-.8-2.4 0` and `c-.8 1.2-1.9 2.1-3.5 2.1` become
# `c-1.6 0-2.7-.45-3.5-1.05`, `c-.6-.4-1.8-.4-2.4 0` and `c-.8.6-1.9 1.05-3.5 1.05`). The nav icon is untouched.
GLASS_PATH = ("M2.5 10.5c0-2.5 2-4.5 4.5-4.5h10c2.5 0 4.5 2 4.5 4.5v1.2c0 3-2.1 5.3-4.8 5.3-1.6 0-2.7-.45-3.5-1.05"
              "-.6-.4-1.8-.4-2.4 0-.8.6-1.9 1.05-3.5 1.05-2.7 0-4.8-2.3-4.8-5.3z")
GLASS_VIEW = (1.2, 4.9, 21.6, 13.2)
# The chart's screen is clipped to the same path, scaled a little smaller so the outline's stroke stays visible and
# its curves cut into the screen's corners and edges.
GLASS_CLIP_SCALE = 0.98


def glass_svg() -> str:
    x, y, w, h = GLASS_VIEW
    inset = f"translate(.5 .5) scale({GLASS_CLIP_SCALE}) translate(-.5 -.5) scale({1 / w:.6f} {1 / h:.6f}) translate({-x} {-y})"
    return (f'<svg class="glass-shape" viewBox="{x} {y} {w} {h}" preserveAspectRatio="none" aria-hidden="true">'
            f'<defs><clipPath id="glass-clip" clipPathUnits="objectBoundingBox"><path transform="{inset}" d="{GLASS_PATH}"/></clipPath></defs>'
            f'<path d="{GLASS_PATH}"/></svg>')


def slug(spec: dict) -> str:
    return re.sub(r"^\d+-", "", spec["name"])


def screen(chart: str, frame: str) -> str:
    inner = f'<div class="bench-chart">{chart}</div>'
    return f'<div class="glass-screen">{inner}</div>' if "glass" in frame else inner


def spec_payload(specs: list[dict]) -> str:
    keep = ("heading", "paragraph", "empty", "box", "series_labels", "chart", "query", "rename")
    payload = [{"slug": slug(spec), **{key: spec[key] for key in keep if key in spec}} for spec in specs]
    return json.dumps(payload, ensure_ascii=False, indent=1).replace("</", "<\\/")


def anchor(text: str, url: str) -> str:
    """A build's label, linked to its release page when the data names one."""
    return (f'<a href="{escape(url)}" rel="noopener">{escape(text)}</a>' if url else escape(text))


def build_item(entry: dict, reasons: dict) -> str:
    """One build's row. A build with no data for the chart is dimmed and disabled, tagged briefly; the tag opens the full
    reason in place (a `details` element, so it works by keyboard, by touch and without scripts), and the row's tooltip
    carries it too. Mirrored by `buildItem` in benchmarks.js."""
    meta = f"build {entry['build'] or 'unknown'}, {entry['date']}"
    link = anchor(entry["label"], entry.get("url", ""))
    key = escape(charts.entry_key(entry))
    if entry["value"] is not None:
        checked = " checked" if entry["default"] else ""
        return (f'<div class="build"><label class="build-label"><input type="checkbox" value="{key}"{checked}> '
                f'<span class="build-name">{link}</span> <span class="build-meta">{escape(meta)}</span></label></div>')
    tag = "not supported" if entry["kind"] == "not_supported" else "not measured here"
    reason = charts.plain_reason(entry["reason"], reasons)
    return (f'<div class="build off" title="{escape(reason, quote=True)}"><label class="build-label"><input type="checkbox" value="{key}" disabled> '
            f'<span class="build-name">{link}</span> <span class="build-meta">{escape(meta)}</span></label>'
            f'<details class="why"><summary class="tag">{tag}</summary><p>{escape(reason)}</p></details></div>')


def builds_list(built: dict, reasons: dict) -> str:
    """The Builds box: one group per port with its newest version, the port's other versions folded away."""
    parts = ["<legend>Builds</legend>"]
    for group in charts.build_groups(built):
        parts.append('<div class="build-group">' + build_item(group["primary"], reasons))
        if group["others"]:
            count = len(group["others"])
            opened = " open" if any(e["default"] for e in group["others"]) else ""
            parts.append(f'<details class="older" data-port="{escape(group["port"])}"{opened}><summary>{count} other version{"" if count == 1 else "s"}</summary>'
                         + "".join(build_item(e, reasons) for e in group["others"]) + "</details>")
        parts.append("</div>")
    return "".join(parts)


def caption_html(table: dict | None) -> str:
    """The table under a chart. Mirrored by `drawCaption` in benchmarks.js."""
    if table is None:
        return ""
    head = "".join(f"<th scope=\"col\">{escape(text)}</th>" for text in ["Build", "Measured", *table["headings"], "Notes"])
    body = ""
    for row in table["rows"]:
        cells = "".join(f'<td data-label="{escape(name, quote=True)}">{escape(cell)}</td>' for name, cell in zip(table["headings"], row["cells"]))
        body += (f'<tr><th scope="row">{anchor(row["label"], row["url"])} <span class="build-meta">build {escape(row["build"])}</span></th>'
                 f'<td data-label="Measured">{escape(row["measured"])}</td>{cells}<td data-label="Notes">{escape(row["notes"])}</td></tr>')
    note = f" {escape(table['note'])}" if table["note"] else ""
    return (f'<table class="bench-table"><caption>{escape(table["box"])}.{note}</caption>'
            f'<thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>')


def section(spec: dict, rows: list[dict], suite: str, reasons: dict) -> str:
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
    rows_html = builds_list(built, reasons)
    swatches = ""
    if built["entries"]:
        swatches = "  ".join(f'<span class="swatch {kind}" aria-hidden="true"></span> {escape(label)}'
                             for kind, label in zip(("avg", "low"), spec["series_labels"][:(2 if charts.has_lows(built) else 1)]))
    return f"""<section class="bench" data-platform="{name}" aria-labelledby="h-{name}">
<h2 id="h-{name}">{escape(spec["heading"])}</h2>
<p>{escape(spec["paragraph"].strip())}</p>
<div class="bench-grid">
<div class="bench-media"><figure class="device {FRAMES[name]}">{glass_svg() if "glass" in FRAMES[name] else ""}{screen(chart, FRAMES[name])}</figure>
<p class="bench-legend">{swatches}</p>
<p class="bench-direction">{escape(charts.direction_note(spec, built))}</p>
<div class="bench-caption">{caption_html(charts.caption_table(built, None, spec["query"].get("details"), spec["query"].get("windows")))}</div></div>
<fieldset class="bench-rows"{"" if built["entries"] else " hidden"}>{rows_html}</fieldset>
</div>
</section>"""


def render_site_page(summary: dict, rows: list[dict], specs: list[dict], legal_html: str) -> str:
    md = MarkdownIt("commonmark", {"html": False})
    reasons = charts.load_reasons()
    suites = "".join(
        f'<label><input type="radio" name="suite" value="{name}"{" checked" if name == charts.DEFAULT_SUITE else ""}> '
        f"{charts.SUITE_NAMES[name]}</label>" for name in charts.SUITE_CHOICES)
    intro = md.render(f"{CAVEAT}\n\nLast measured: {last_measured(summary, rows)}. The "
                      f"[results spreadsheet]({SHEET_URL}) holds every number. The methodology is at the bottom of this page.")
    method = md.render("\n".join(methodology(summary, rows)))
    body = "\n".join(section(spec, rows, charts.DEFAULT_SUITE, reasons) for spec in specs)
    head = head_tags({"description": DESCRIPTION, "og_title": TITLE, "path": "benchmarks.html"})
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{TITLE}</title>
<meta name="description" content="{DESCRIPTION}">
<meta name="color-scheme" content="light dark">
{head["head_icons"]}
<link rel="stylesheet" href="style.css">
{head["head_social"]}
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
<script type="application/json" id="chart-reasons">{json.dumps(reasons, ensure_ascii=False)}</script>
<script src="{MERMAID}" integrity="{MERMAID_SRI}" crossorigin="anonymous"></script>
<script src="benchmarks/benchmarks.js"></script>
</body>
</html>
"""
