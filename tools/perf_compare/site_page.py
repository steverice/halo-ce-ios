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


def site_header() -> str:
    """The brand and site navigation shared with the home page (tools/site_header.html, filled by build_site.py)."""
    return Template((ROOT / "tools/site_header.html").read_text(encoding="utf-8")).substitute(
        home_current="", bench_current=' aria-current="page"').rstrip("\n")


def suite_info(name: str, info: dict) -> str:
    """The selected suite's description and the screenshots of the load each of its windows runs. The Suite bar shows
    only the panel of the checked suite (CSS), so it needs no script."""
    shots = "".join(
        "<li>" + (f'<img src="benchmarks/thumbs/{escape(w["shot"])}.jpg" width="64" height="40" alt="" loading="lazy" decoding="async">'
                  if w.get("shot") else "") + f'<span>{escape(w["label"])}</span></li>' for w in info.get("windows", []))
    return (f'<div class="suite-info" data-suite="{name}"><p class="suite-text">{escape(info["description"])}</p>'
            + (f'<ul class="suite-shots" aria-label="Windows sampled">{shots}</ul>' if shots else "") + "</div>")


def head_tags(metadata: dict[str, str]) -> dict[str, str]:
    """The icon and social-card tags shared with the other site pages (tools/site_head_*.html, read by build_site.py)."""
    icons = (ROOT / "tools/site_head_icons.html").read_text(encoding="utf-8").rstrip("\n")
    social = Template((ROOT / "tools/site_head_social.html").read_text(encoding="utf-8")).substitute(
        {key: escape(value, quote=True) for key, value in metadata.items()}).rstrip("\n")
    return {"head_icons": icons, "head_social": social}


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


def build_item(entry: dict, reasons: dict, off: dict | None = None, checked: bool = False) -> str:
    """One build's row. A build the chart does not draw (`off`, from `charts.list_status`) is grayed out and cannot be
    ticked, tagged briefly; the tag opens the full reason in place (a `details` element, so it works by keyboard, by
    touch and without scripts), and the row's tooltip carries it too. Mirrored by `buildItem` in benchmarks.js."""
    meta = f"build {entry['build'] or 'unknown'}, {entry['date']}"
    link = anchor(entry["label"], entry.get("url", ""))
    key = escape(charts.entry_key(entry))
    if off is None:
        mark = " checked" if checked else ""
        return (f'<div class="build"><label class="build-label"><input type="checkbox" value="{key}"{mark}> '
                f'<span class="build-name">{link}</span> <span class="build-meta">{escape(meta)}</span></label></div>')
    reason = charts.plain_reason(off["reason"], reasons)
    return (f'<div class="build off" title="{escape(reason, quote=True)}"><label class="build-label"><input type="checkbox" value="{key}" disabled> '
            f'<span class="build-name">{link}</span> <span class="build-meta">{escape(meta)}</span></label>'
            f'<details class="why"><summary class="tag">{off["tag"]}</summary><p>{escape(reason)}</p></details></div>')


def builds_list(built: dict, reasons: dict, selected: set[str] | None = None) -> str:
    """The Builds box: one group per port with its newest version, the port's other versions folded away. Its ticked,
    available rows are the builds the chart draws."""
    status = charts.list_status(built, selected)

    def item(entry: dict) -> str:
        key = charts.entry_key(entry)
        return build_item(entry, reasons, status.get(key), key not in status and (key in selected if selected is not None else entry["default"]))

    parts = ["<legend>Builds</legend>"]
    for group in charts.build_groups(built):
        parts.append('<div class="build-group">' + item(group["primary"]))
        if group["others"]:
            count = len(group["others"])
            opened = " open" if any(e["default"] and charts.entry_key(e) not in status for e in group["others"]) else ""
            parts.append(f'<details class="older" data-port="{escape(group["port"])}"{opened}><summary>{count} other version{"" if count == 1 else "s"}</summary>'
                         + "".join(item(e) for e in group["others"]) + "</details>")
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
    info = charts.load_suite_info()
    suites = "".join(
        f'<label><input type="radio" name="suite" value="{name}"{" checked" if name == charts.DEFAULT_SUITE else ""}> '
        f"{charts.SUITE_NAMES[name]}</label>" for name in charts.SUITE_CHOICES)
    intro = md.render(f"{CAVEAT}\n\nLast measured: {last_measured(summary, rows)}.\n\nThe "
                      f"[results spreadsheet]({SHEET_URL}) holds every number. The methodology is at the bottom of this page.")
    panels = "".join(suite_info(name, info[name]) for name in charts.SUITE_CHOICES)
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
</head>
<body>
{site_header()}
<main class="page page-bench" id="bench-root" data-csv="benchmarks/results.csv">
<h1>Performance benchmarks</h1>
{intro.rstrip()}
<fieldset class="bench-bar"><legend>Suite</legend><div class="suite-pills">{suites}</div>{panels}</fieldset>
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
