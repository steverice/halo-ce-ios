"""Sheet export, snapshot fetch, chart generation and page composition."""

from copy import deepcopy
import csv
import io
import json
import shutil
import subprocess
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from perf_compare import charts, page, sheet_export, sheet_fetch, site_page

ROOT = Path(__file__).resolve().parents[1]


def run(round_, scenario, fps, p99=19.0, machine="Mac mini (M4, 16 GB)"):
    return {"round": round_, "scenario": scenario, "machine": machine, "os": "macOS 27", "resolution": "1280×960",
            "cap": "60", "display_hz": 60, "thermal": "nominal → nominal",
            "metrics": {"fps": fps, "p50_ms": 16.6, "p95_ms": 18.0, "p99_ms": p99, "on_time_pct": 90.0,
                        "worst_ms": 40.0, "frames": 3600, "duration_s": 60.0}}


def record(port="visr", version="0.5.1", mode="capped", delay=0, date="2026-10-26", rounds=3, **extra):
    base = {"port": port, "version": version, "build": "7", "source": "abc123", "machine": "Mac mini (M4, 16 GB)",
            "os": "macOS 27", "browser": "", "method": "metalperftrace on-glass intervals", "frame_mode": mode,
            "native_delay_seconds": delay, "quality_preset": "defaults", "warmup_seconds": 15, "date": date,
            "status": "ok", "release_kind": "release", "artifact": "release build", "sha256": "ff", "scenarios": ["a10"],
            "runs": [run(n, "a10", fps) for n, fps in zip(range(1, rounds + 1), (58.0, 60.0, 62.0))]}
    base.update(extra)
    return base


def summary(*records, **extra):
    return {"schema": 2, "records": list(records), **extra}


def test_rows_aggregate_rounds_and_keep_column_order():
    [row] = sheet_export.rows(summary(record()))
    assert list(row) == list(sheet_export.COLUMNS)
    assert (row["host"], row["chip"], row["memory_gb"], row["machine"]) == ("M4 Mac mini", "M4", "16", "Mac mini")
    assert (row["fps_avg"], row["fps_min"], row["fps_max"], row["rounds"], row["status"]) == ("60.00", "58.00", "62.00", 3, "ok")
    assert row["label"] == "VISR 0.5.1" and row["window"] == "a10" and row["on_time_pct"] == "90.0"


def test_uncapped_rows_omit_cap_and_on_time_and_name_the_delayed_window():
    rec = record(mode="uncapped", delay=90, scenarios=["b30"])
    for item in rec["runs"]:
        item["scenario"] = "b30"
    [row] = sheet_export.rows(summary(rec))
    assert (row["window"], row["cap_fps"], row["on_time_pct"], row["delay_s"]) == ("b30 +90 s", "", "", 90)


def test_unavailable_and_incomplete_sets_stay_visible():
    gone = record(port="opence-web", version="", runs=[], status="unavailable", reason="safari-remote-automation-disabled")
    rows = sheet_export.rows(summary(gone, record(rounds=2)))
    by_port = {row["port"]: row for row in rows}
    assert by_port["opence-web"]["status"] == "unavailable" and by_port["opence-web"]["fps_avg"] == ""
    assert by_port["opence-web"]["notes"] == "safari-remote-automation-disabled"
    assert by_port["visr"]["status"] == "incomplete" and by_port["visr"]["rounds"] == 2


def test_csv_round_trips_with_a_stable_header_and_no_private_names():
    text = sheet_export.to_csv(sheet_export.COLUMNS, sheet_export.rows(summary(record())))
    assert text.splitlines()[0] == ",".join(sheet_export.COLUMNS)
    assert list(csv.DictReader(io.StringIO(text)))[0]["port"] == "visr"
    assert "\r" not in text


def test_every_column_is_documented():
    assert set(sheet_export.DESCRIPTIONS) == set(sheet_export.COLUMNS)


def test_layout_filters_each_port_over_the_results_tab():
    layout = sheet_export.layout(summary(record(), record(port="corece", version="0.3.0")))
    tabs = {tab["name"]: tab for tab in layout["tabs"]}
    assert list(tabs)[:3] == ["README", "charts", "results"]
    assert tabs["visr"]["formula"] .startswith("={results!A1:A")
    assert "corece" in tabs and "device-sweep" not in tabs
    last = sheet_export._column_letter(len(sheet_export.COLUMNS) - 1)
    assert f"results!A1:{last}1" in tabs["visr"]["formula"]


def test_summary_schema_is_checked():
    with pytest.raises(ValueError):
        sheet_export.rows({"schema": 1})


def box_record(port="visr", version="0.5.1", build="7", date="2026-10-26", mode="uncapped", fps=(500.0, 600.0), p99=5.0,
               **extra):
    """A matched-stock campaign set on the Mac reference box with three rounds per window."""
    rec = record(port=port, version=version, mode=mode, date=date, build=build, machine="Mac mini (M6, 24 GB)",
                 quality_preset="matched-stock", scenarios=["a10", "b30"], **extra)
    rec["runs"] = [run(n, scenario, value, p99, "Mac mini (M6, 24 GB)")
                   for n in (1, 2, 3) for scenario, value in (("a10", fps[0]), ("b30", fps[1]))]
    return rec


def csv_rows(rows):
    """Rows as the charts see them: read back from CSV, every value a string."""
    return list(csv.DictReader(io.StringIO(sheet_export.to_csv(sheet_export.COLUMNS, rows))))


def snapshot_rows(*extra):
    return csv_rows(sheet_export.rows(summary(
        box_record(), box_record(version="main", build="5", date="2026-10-24"),
        box_record(port="opence-macos", version="v1.0.2", build="2"),
        record(port="opence-web", version="", runs=[], status="unavailable", reason="safari-remote-automation-disabled"),
        *extra)))


QUERY = {"platform": "Mac", "metrics": ["fps_avg", "fps_1pct_low"], "port_order": ["visr", "opence-macos", "opence-web"],
         "no_data_reason": "no uncapped matched-stock measurement",
         "filter": {"frame_mode": "uncapped", "delay_s": "0", "quality_preset": "matched-stock", "status": "ok"}}


def spec(**overrides):
    base = {"name": "1-mac", "heading": "Mac", "paragraph": "P.", "empty": "No Mac results yet.", "box": "M6 Mac mini",
            "series_labels": ["Average fps", "1% low fps"], "rename": {"OpenCE-macOS": "OpenCE"},
            "chart": {"type": "bar", "horizontal": True, "x_label": "X", "y_label": "Y", "marks": ["A", "B"],
                      "header": "---\nconfig: {}\n---"},
            "query": QUERY}
    base.update(overrides)
    return base


def test_platform_and_reference_box_columns():
    rows = {row["host"]: row for row in snapshot_rows(record())}
    assert (rows["M6 Mac mini"]["platform"], rows["M6 Mac mini"]["reference_box"]) == ("Mac", "true")
    assert rows["M4 Mac mini"]["reference_box"] == "false"
    assert [sheet_export.platform_of(m) for m in ("iPad Pro", "iPhone 17 Pro", "Apple TV 4K", "Apple Vision Pro")] == [
        "iPhone/iPad", "iPhone/iPad", "Apple TV", "Vision Pro"]


def test_every_row_has_a_suite_and_rejected_attempts_get_their_own_rows():
    rec = box_record(rejected=[{"scenario": "a10", "round": 2, "reason": "background CPU"}])
    rows = sheet_export.rows(summary(rec))
    assert {row["suite"] for row in rows} == {"campaign"}
    [gone] = [row for row in rows if row["status"] == "rejected"]
    assert gone["notes"] == "round 2: background CPU" and gone["fps_avg"] == "" and gone["rounds"] == 0
    assert sum(row["status"] == "ok" for row in rows) == 2


def test_not_supported_is_its_own_status():
    rec = box_record(port="corece", version="0.3.0", status="not_supported", reason="Does not support split screen",
                     suite="co-op", runs=[])
    [row] = [r for r in sheet_export.rows(summary(rec)) if r["scenario"] == "a10"]
    assert (row["status"], row["suite"], row["notes"]) == ("not_supported", "co-op", "Does not support split screen")


def test_one_percent_low_is_the_rate_of_the_99th_percentile_interval():
    row = [r for r in snapshot_rows() if r["port"] == "visr" and r["version"] == "0.5.1"][0]
    assert row["fps_1pct_low"] == "200.00"  # 1000 / 5 ms


def test_model_ticks_the_newest_build_of_each_port_and_lists_the_rest_with_reasons():
    built = charts.model(snapshot_rows(), QUERY, "campaign")
    by_key = {charts.entry_key(e): e for e in built["entries"]}
    assert built["box"] == "M6 Mac mini"
    assert by_key["visr@0.5.1@7"]["default"] and not by_key["visr@main@5"]["default"]
    assert by_key["visr@main@5"]["value"] is not None
    assert by_key["opence-web@@7"]["reason"] == "safari-remote-automation-disabled"
    assert by_key["opence-web@@7"]["kind"] == "not_measured"
    assert by_key["visr@0.5.1@7"]["value"] == {"avg": pytest.approx(547.7226, abs=1e-3), "low": pytest.approx(200.0), "rows": by_key["visr@0.5.1@7"]["value"]["rows"]}


def test_suites_without_data_show_not_measured_rows_and_overall_needs_every_suite():
    built = charts.model(snapshot_rows(), QUERY, "multiplayer")
    assert all(e["value"] is None for e in built["entries"])
    assert {e["reason"] for e in built["entries"] if e["port"] == "visr"} == {"no multiplayer measurements yet"}
    overall = charts.model(snapshot_rows(), QUERY, "overall")
    assert {e["reason"] for e in overall["entries"] if e["port"] == "visr"} == {"missing multiplayer, co-op"}
    assert not any(e["default"] for e in overall["entries"])


def test_overall_is_the_geometric_mean_of_each_metric_with_equal_suite_weight():
    extra = [{**box_record(fps=(100.0, 100.0), p99=10.0), "suite": "multiplayer"},
             {**box_record(fps=(300.0, 300.0), p99=20.0), "suite": "co-op"}]
    built = charts.model(snapshot_rows() + csv_rows(sheet_export.rows(summary(*extra))), QUERY, "overall")
    value = [e for e in built["entries"] if e["port"] == "visr" and e["version"] == "0.5.1"][0]["value"]
    assert value["avg"] == pytest.approx((500 ** 0.5 * 600 ** 0.5 * 100 * 300) ** (1 / 3))
    assert value["low"] == pytest.approx((200 * 100 * 50) ** (1 / 3))


def test_not_supported_suite_is_reported_as_not_supported_not_not_measured():
    rec = box_record(port="corece", version="0.3.0", status="not_supported", reason="Does not support split screen",
                     suite="co-op", runs=[])
    built = charts.model(snapshot_rows() + csv_rows(sheet_export.rows(summary(rec))), QUERY, "co-op")
    [entry] = [e for e in built["entries"] if e["port"] == "corece"]
    assert (entry["kind"], entry["reason"]) == ("not_supported", "Does not support split screen")
    text = charts.render_section(spec(), snapshot_rows() + csv_rows(sheet_export.rows(summary(rec))), "co-op")
    assert "Not supported: CoreCE 0.3.0 (Does not support split screen)." in text


def test_chart_source_draws_average_then_low_for_the_ticked_builds():
    built = charts.model(snapshot_rows(), QUERY, "campaign")
    text = charts.source(spec(), built, "campaign")
    lines = text.splitlines()
    assert lines[:4] == ["---", "config: {}", "---", "xychart-beta horizontal"]
    assert 'title "Campaign: uncapped fps, M6 Mac mini"' in text
    assert 'x-axis "X" ["VISR 0.5.1", "OpenCE v1.0.2"]' in text
    assert "    bar [547.7, 547.7]" in text and "    bar [200, 200]" in text
    chosen = charts.source(spec(), built, "campaign", {"visr@main@5"})
    assert '["VISR main"]' in chosen
    assert charts.source(spec(), built, "campaign", set()) is None
    assert charts.legend(spec()) == "A Average fps B 1% low fps"
    assert "VISR 0.5.1 (build 7, measured 2026-10-26)" in charts.caption(built)


def test_platform_without_builds_shows_its_explanation_not_a_chart():
    text = charts.render_section(spec(query={**QUERY, "platform": "Vision Pro"}), snapshot_rows(), "campaign")
    assert "No Mac results yet." in text and "```mermaid" not in text


def test_apply_replaces_only_the_marked_block_and_is_idempotent():
    text = f"top\n{charts.BEGIN}\nold\n{charts.END}\nbottom\n"
    once = charts.apply(text, "new")
    assert once.startswith("top\n") and once.endswith("bottom\n") and "old" not in once and "new" in once
    assert charts.apply(once, "new") == once
    with pytest.raises(ValueError):
        charts.apply("no markers", "new")


def test_y_axis_ceiling_gives_headroom():
    assert charts._nice_ceiling(64.8) == 80 and charts._nice_ceiling(0) == 1


def test_fetch_accepts_the_results_header_and_normalizes_line_ends():
    text = sheet_export.to_csv(sheet_export.COLUMNS, snapshot_rows()).replace("\n", "\r\n")
    assert "\r" not in sheet_fetch.validate("\ufeff" + text)


@pytest.mark.parametrize("text", ["<!DOCTYPE html><html>sign in", "a,b\n1,2\n", ",".join(sheet_export.COLUMNS) + "\n"])
def test_fetch_rejects_pages_wrong_tabs_and_empty_results(text):
    with pytest.raises(ValueError):
        sheet_fetch.validate(text)


def test_export_url_names_the_tab():
    assert sheet_fetch.export_url("ID", "12") == "https://docs.google.com/spreadsheets/d/ID/export?format=csv&gid=12"


def test_markdown_page_has_charts_first_methodology_last_and_no_result_tables():
    data = summary(box_record(), box_record(port="opence-macos", version="v1.0.2", build="2"))
    rows = csv_rows(sheet_export.rows(data))
    text = page.render_page(data, rows, [spec()])
    assert text.index("## Results") < text.index("```mermaid") < text.index("## Methodology")
    assert page.SHEET_URL in text and page.SITE_PAGE_URL in text and "—" not in text
    assert "### Suites" in text and "not_supported" in text and "0 rejected attempts" in text
    assert not any(line.startswith("| ") for line in text.splitlines())


def test_site_page_has_a_default_view_suite_switch_and_the_pinned_mermaid():
    data = summary(box_record())
    html = site_page.render_site_page(data, csv_rows(sheet_export.rows(data)), [spec()], "<p>legal</p>")
    assert 'name="suite" value="overall"' in html and 'value="campaign" checked' in html
    assert f'src="{site_page.MERMAID}"' in html and "mermaid@11.12.0" in site_page.MERMAID
    assert site_page.MERMAID_SRI.startswith("sha384-") and 'crossorigin="anonymous"' in html
    assert 'type="checkbox" value="visr@0.5.1@7" checked' in html
    assert '"slug": "mac"' in html and "</script><script" not in html


def test_committed_snapshot_has_the_export_columns_and_every_spec_renders_from_it():
    rows = charts.load_snapshot(ROOT / "docs/benchmarks/results.csv")
    assert rows and list(rows[0]) == list(sheet_export.COLUMNS)
    for item in charts.load_specs(ROOT / "docs/benchmarks/charts"):
        for suite in charts.SUITE_CHOICES:
            assert charts.render_section(item, rows, suite).startswith("### ")


def test_pages_copy_of_the_snapshot_is_the_committed_snapshot():
    assert (ROOT / "docs/site/benchmarks/results.csv").read_bytes() == (ROOT / "docs/benchmarks/results.csv").read_bytes()


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_browser_model_draws_the_same_charts_as_python_for_every_suite():
    rows = charts.load_snapshot(ROOT / "docs/benchmarks/results.csv")
    specs = charts.load_specs(ROOT / "docs/benchmarks/charts")
    script = """
const fs = require('fs');
const m = require(process.argv[1] + '/docs/site/benchmarks/benchmarks.js');
const rows = m.parseCsv(fs.readFileSync(process.argv[1] + '/docs/benchmarks/results.csv', 'utf8'));
const out = {};
for (const spec of JSON.parse(fs.readFileSync(0, 'utf8'))) for (const suite of ['campaign', 'multiplayer', 'co-op', 'overall']) {
  const built = m.model(rows, spec.query, suite);
  out[spec.slug + '/' + suite] = [m.chartSource(spec, built, suite), m.captionText(built), built.entries.map((e) => [m.entryKey(e), e.default, e.kind, e.reason])];
}
console.log(JSON.stringify(out));
"""
    payload = json.dumps([{**item, "slug": site_page.slug(item)} for item in specs])
    done = subprocess.run(["node", "-e", script, str(ROOT)], input=payload, capture_output=True, text=True, check=True)
    browser = json.loads(done.stdout)
    for item in specs:
        for suite in charts.SUITE_CHOICES:
            built = charts.model(rows, item["query"], suite)
            expected = [charts.source(item, built, suite), charts.caption(built),
                        [[charts.entry_key(e), e["default"], e["kind"], e["reason"]] for e in built["entries"]]]
            assert browser[f"{site_page.slug(item)}/{suite}"] == expected


def test_fetch_lowercases_sheets_booleans_and_keeps_dropped_zeros_readable():
    rows = snapshot_rows()
    for row in rows:
        row["reference_box"] = row["reference_box"].upper()
        row["fps_avg"] = row["fps_avg"].rstrip("0").rstrip(".") if row["fps_avg"] else ""
    text = sheet_export.to_csv(sheet_export.COLUMNS, rows)
    cleaned = list(csv.DictReader(io.StringIO(sheet_fetch.validate(text))))
    assert {row["reference_box"] for row in cleaned} <= {"true", "false"}
    model = charts.model(cleaned, QUERY, "campaign")
    assert any(e["default"] for e in model["entries"])
    assert model["box"] == "M6 Mac mini"


def test_readme_tab_describes_every_column_in_order():
    rows = list(csv.DictReader(io.StringIO(sheet_export.readme_csv())))
    assert [row["column"] for row in rows] == list(sheet_export.COLUMNS) and all(row["meaning"] for row in rows)


def test_nothing_from_the_device_sweep_reaches_the_public_outputs():
    assert not hasattr(sheet_export, "sweep_rows")
    layout = sheet_export.layout(summary(record(), sweep=[{"device": "iPad"}]))
    assert "device-sweep" not in {tab["name"] for tab in layout["tabs"]}
    assert "device-sweep" not in page.render_page(summary(record(), sweep=[{"device": "iPad"}]), [], [])


def test_label_is_the_first_column_and_port_filters_sit_on_column_b():
    assert sheet_export.COLUMNS[:2] == ("label", "port")
    layout = sheet_export.layout(summary(record()))
    assert 'results!B2:B="visr"' in {tab["name"]: tab for tab in layout["tabs"]}["visr"]["formula"]


def test_fetch_reads_columns_by_name_so_a_reordered_sheet_works():
    rows = snapshot_rows()[:3]
    shuffled = list(reversed(sheet_export.COLUMNS)) + ["sheet_only"]
    text = sheet_export.to_csv(shuffled, [{**row, "sheet_only": "x"} for row in rows])
    cleaned = list(csv.DictReader(io.StringIO(sheet_fetch.validate(text))))
    assert list(cleaned[0]) == list(sheet_export.COLUMNS)
    assert [r["label"] for r in cleaned] == [r["label"] for r in rows]
    with pytest.raises(ValueError, match="lacks results columns: window"):
        sheet_fetch.validate(sheet_export.to_csv([c for c in sheet_export.COLUMNS if c != "window"],
                                                 [{k: v for k, v in rows[0].items() if k != "window"}]))
