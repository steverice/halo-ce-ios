#!/usr/bin/env python3
"""Pre-render the default view of each headline chart to SVG, the page's no-JavaScript fallback.

Needs Playwright with Chromium (`uv run --with playwright python tools/perf_compare/static_charts.py`) and
network access for the pinned Mermaid script. Not part of CI: rerun it and commit the SVGs when the snapshot or a
chart spec changes. `refresh.py render` then shows each SVG as an `<img>`.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import charts
from site_page import MERMAID, SITE, slug


def main() -> int:
    from playwright.sync_api import sync_playwright
    rows, specs = charts.load_snapshot(charts.SNAPSHOT), charts.load_specs(charts.SPECS)
    (SITE / "benchmarks").mkdir(exist_ok=True)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        page = browser.new_page()
        page.set_content("<!doctype html><body></body>")
        page.add_script_tag(url=MERMAID)
        page.evaluate("mermaid.initialize({startOnLoad:false})")
        for spec in specs:
            text = charts.source(spec, charts.model(rows, spec["query"], charts.DEFAULT_SUITE), charts.DEFAULT_SUITE)
            target = SITE / f"benchmarks/static-{slug(spec)}.svg"
            if text is None:
                target.unlink(missing_ok=True)
                continue
            svg = page.evaluate("async (s) => (await mermaid.render('static', s)).svg", text)
            target.write_text(svg + "\n", encoding="utf-8")
            print("wrote", target.relative_to(charts.ROOT))
        browser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
