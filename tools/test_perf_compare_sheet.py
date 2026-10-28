"""Sheet export, snapshot fetch, chart generation and page composition."""

from copy import deepcopy
import csv
import re
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
    assert by_key["visr@0.5.1@7"]["value"] == {"avg": pytest.approx(547.7226, abs=1e-3), "low": pytest.approx(200.0), "details": {}, "windows": ["a10", "b30"], "rows": by_key["visr@0.5.1@7"]["value"]["rows"]}


def test_suites_without_data_show_not_measured_rows():
    built = charts.model(snapshot_rows(), QUERY, "multiplayer")
    assert all(e["value"] is None for e in built["entries"])
    assert {e["reason"] for e in built["entries"] if e["port"] == "visr"} == {"no multiplayer measurements yet"}


def overall_rows(*extra):
    return snapshot_rows(*extra)


def overall_entry(rows, port, version, selected=None):
    built = charts.model(rows, QUERY, "overall")
    [entry] = [e for e in charts._drawn(built, selected) if e["port"] == port and e["version"] == version]
    return built, entry["value"]


def test_overall_covers_only_the_suites_every_drawn_build_has_measured():
    # VISR also has Multiplayer and Combat; OpenCE-macOS has Campaign only: the chart compares on Campaign.
    extra = [{**box_record(fps=(100.0, 100.0), p99=10.0), "suite": "multiplayer"},
             {**box_record(fps=(300.0, 300.0), p99=20.0), "suite": "combat"}]
    rows = overall_rows(*[r for r in extra])
    built, visr = overall_entry(rows, "visr", "0.5.1")
    assert charts.covered_suites([e for e in built["entries"] if e["default"] and e["value"]]) == ["campaign"]
    assert visr["avg"] == pytest.approx((500 * 600) ** 0.5)
    assert charts.overall_note(built) == "Overall covers Campaign."
    assert "M6 Mac mini. Overall covers Campaign. " in charts.caption(built)


def test_overall_grows_when_every_drawn_build_gains_a_suite():
    extra = [{**box_record(fps=(100.0, 100.0), p99=10.0), "suite": "multiplayer"},
             {**box_record(port="opence-macos", version="v1.0.2", build="2", fps=(50.0, 50.0), p99=40.0), "suite": "multiplayer"}]
    built, visr = overall_entry(overall_rows(*extra), "visr", "0.5.1")
    assert charts.overall_note(built) == "Overall covers Campaign and Multiplayer."
    assert visr["avg"] == pytest.approx(((500 * 600) ** 0.5 * 100) ** 0.5)
    assert visr["low"] == pytest.approx((200 * 100) ** 0.5)
    _, other = overall_entry(overall_rows(*extra), "opence-macos", "v1.0.2")
    assert other["avg"] == pytest.approx(((500 * 600) ** 0.5 * 50) ** 0.5)


def test_overall_coverage_follows_the_ticked_builds():
    extra = [{**box_record(fps=(100.0, 100.0), p99=10.0), "suite": "multiplayer"}]
    rows = overall_rows(*extra)
    built = charts.model(rows, QUERY, "overall")
    assert charts.overall_note(built, {"visr@0.5.1@7"}) == "Overall covers Campaign and Multiplayer."
    assert charts.overall_note(built, {"visr@0.5.1@7", "opence-macos@v1.0.2@2"}) == "Overall covers Campaign."
    text = charts.source(spec(), built, "overall", {"visr@0.5.1@7"})
    assert 'title "Overall: uncapped fps, M6 Mac mini"' in text and '["VISR 0.5.1"]' in text
    assert charts.source(spec(), built, "overall", set()) is None


def test_overall_with_no_common_suite_draws_nothing_and_says_why():
    rows = snapshot_rows(
        {**box_record(port="corece", version="0.3.0", build="9", fps=(100.0, 100.0)), "suite": "co-op"})
    built = charts.model(rows, {**QUERY, "port_order": ["visr", "corece"]}, "overall")
    keys = {"visr@0.5.1@7", "corece@0.3.0@9"}
    assert charts.source(spec(), built, "overall", keys) is None
    assert charts.caption(built, keys) == ""
    # The default view ticks each port's newest build: VISR and OpenCE-macOS share Campaign, CoreCE only has Co-op.
    assert charts.source(spec(), built, "overall") is None
    assert charts.empty_text(spec(), built) == "No suite has been measured on every build in this chart."


def test_overall_lists_builds_with_no_data_and_defaults_to_the_newest_build_of_each_port():
    built = charts.model(snapshot_rows(), QUERY, "overall")
    by_key = {charts.entry_key(e): e for e in built["entries"]}
    assert by_key["opence-web@@7"]["value"] is None and by_key["opence-web@@7"]["reason"]
    assert by_key["visr@0.5.1@7"]["default"] and not by_key["visr@main@5"]["default"]


def test_not_supported_suite_is_reported_as_not_supported_not_not_measured():
    rec = box_record(port="corece", version="0.3.0", status="not_supported", reason="Does not support split screen",
                     suite="co-op", runs=[])
    built = charts.model(snapshot_rows() + csv_rows(sheet_export.rows(summary(rec))), QUERY, "co-op")
    [entry] = [e for e in built["entries"] if e["port"] == "corece"]
    assert (entry["kind"], entry["reason"]) == ("not_supported", "Does not support split screen")
    text = charts.render_section(spec(), snapshot_rows() + csv_rows(sheet_export.rows(summary(rec))), "co-op")
    assert "Not supported: [CoreCE 0.3.0](https://github.com/mastercheese-dev/CoreCE/releases/tag/build-7) (Does not support split screen)." in text


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
for (const spec of JSON.parse(fs.readFileSync(0, 'utf8'))) for (const suite of m.SUITES.concat(['overall'])) {
  const built = m.model(rows, spec.query, suite);
  out[spec.slug + '/' + suite] = [m.chartSource(spec, built, suite), m.captionText(built, null, spec.query.details, spec.query.windows), built.entries.map((e) => [m.entryKey(e), e.default, e.kind, e.reason])];
  const firstKey = built.entries.length ? new Set([m.entryKey(built.entries[0])]) : new Set();
  out[spec.slug + '/' + suite + '/first'] = [m.chartSource(spec, built, suite, firstKey), m.captionText(built, firstKey, spec.query.details, spec.query.windows)];
}
console.log(JSON.stringify(out));
"""
    payload = json.dumps([{**item, "slug": site_page.slug(item)} for item in specs])
    done = subprocess.run(["node", "-e", script, str(ROOT)], input=payload, capture_output=True, text=True, check=True)
    browser = json.loads(done.stdout)
    for item in specs:
        for suite in charts.SUITE_CHOICES:
            built = charts.model(rows, item["query"], suite)
            expected = [charts.source(item, built, suite), charts.caption(built, None, item["query"].get("details"), "", item["query"].get("windows")),
                        [[charts.entry_key(e), e["default"], e["kind"], e["reason"]] for e in built["entries"]]]
            assert browser[f"{site_page.slug(item)}/{suite}"] == expected
            first = {charts.entry_key(built["entries"][0])} if built["entries"] else set()
            assert browser[f"{site_page.slug(item)}/{suite}/first"] == [
                charts.source(item, built, suite, first), charts.caption(built, first, item["query"].get("details"), "", item["query"].get("windows"))]


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


def test_reason_codes_become_plain_sentences_and_unknown_text_passes_through():
    reasons = charts.load_reasons()
    assert charts.plain_reason("safari-remote-automation-disabled", reasons) == "Safari could not be automated on this Mac."
    assert charts.plain_reason("no co-op measurements yet", reasons) == "The co-op suite has not been run for this build."
    assert charts.plain_reason("missing multiplayer, co-op", reasons) == "No overall result: missing multiplayer, co-op."
    assert charts.plain_reason("Does not support split screen", reasons) == "Does not support split screen"


def test_every_reason_code_in_the_committed_data_has_a_plain_sentence():
    rows = charts.load_snapshot(ROOT / "docs/benchmarks/results.csv")
    reasons = charts.load_reasons()
    for item in charts.load_specs(ROOT / "docs/benchmarks/charts"):
        for suite in charts.SUITE_CHOICES:
            for entry in charts.model(rows, item["query"], suite)["entries"]:
                if entry["value"] is None:
                    assert charts.plain_reason(entry["reason"], reasons) != entry["reason"], entry["reason"]


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_browser_plain_reasons_match_python():
    codes = ["safari-remote-automation-disabled", "console-unavailable", "no multiplayer measurements yet",
             "missing campaign, co-op", "Does not support split screen", "toString"]
    reasons = charts.load_reasons()
    script = ("const m = require(process.argv[1] + '/docs/site/benchmarks/benchmarks.js');"
              "const r = JSON.parse(process.argv[2]); console.log(JSON.stringify(JSON.parse(process.argv[3]).map((c) => m.plainReason(c, r))));")
    done = subprocess.run(["node", "-e", script, str(ROOT), json.dumps(reasons), json.dumps(codes)],
                          capture_output=True, text=True, check=True)
    assert json.loads(done.stdout) == [charts.plain_reason(code, reasons) for code in codes]


def test_only_released_builds_reach_the_public_rows():
    kept = [record(), record(port="opence-web", version="build-280", release_kind="hosted web build (release channel)")]
    dropped = [record(version="main", release_kind="pre-release main build"),
               record(port="opence-web", version="", release_kind="hosted web build")]
    rows = sheet_export.rows(summary(*kept, *dropped))
    assert {(row["port"], row["version"]) for row in rows} == {("visr", "0.5.1"), ("opence-web", "build-280")}
    assert not any(row["release_kind"] == "pre-release main build" for row in rows)
    assert "main" not in {row["version"] for row in rows}


def test_fetch_drops_unreleased_rows_even_if_the_sheet_still_has_them():
    rows = snapshot_rows()
    unreleased = {**rows[0], "label": "VISR dev", "version": "dev", "release_kind": "pre-release main build"}
    text = sheet_export.to_csv(sheet_export.COLUMNS, [*rows, unreleased])
    cleaned = list(csv.DictReader(io.StringIO(sheet_fetch.validate(text))))
    assert "VISR dev" not in {row["label"] for row in cleaned} and len(cleaned) == len(rows)


def test_chart_palette_matches_the_site_tokens_and_the_icon_colors():
    css = (ROOT / "docs/site/style.css").read_text()
    light = [re.search(rf"--{name}:\s*(#[0-9a-f]{{6}})", css).group(1) for name in ("chart-avg", "chart-low")]
    for item in charts.load_specs(ROOT / "docs/benchmarks/charts"):
        assert item["chart"]["palette"] == light
        assert "__PALETTE__" in item["chart"]["header"]
        assert ", ".join(light) in charts.header_text(item["chart"])
    assert "--visr-blue: #1fa9d4" in css and "--visr-gold: #ffc24d" in css


def _luminance(color):
    channels = [int(color[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    r, g, b = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def test_light_mode_bars_keep_three_to_one_contrast_on_white():
    css = (ROOT / "docs/site/style.css").read_text()
    for name in ("chart-avg", "chart-low"):
        color = re.search(rf"--{name}:\s*(#[0-9a-f]{{6}})", css).group(1)
        assert 1.05 / (_luminance(color) + 0.05) >= 3.0, name


def test_home_page_links_to_the_benchmarks_page_and_the_readme_to_its_site_address():
    assert 'href="benchmarks.html"' in (ROOT / "docs/site/index.html").read_text()
    readme = (ROOT / "README.md").read_text()
    assert "(https://code.steverice.org/visr/benchmarks.html)" in readme and "docs/benchmarks.md" not in readme


def test_pages_chart_script_fits_an_even_margin_and_draws_no_gridlines():
    script = (ROOT / "docs/site/benchmarks/benchmarks.js").read_text()
    assert "function finishChart" in script and "finishChartSvg" in script and "gridline" not in script.replace("no gridline", "")


def _method_row(port, port_name, platform, host, method, mode, suite="campaign", cap="60", hz="60", resolution="1280×960"):
    row = dict.fromkeys(sheet_export.COLUMNS, "")
    row.update(port=port, port_name=port_name, platform=platform, host=host, method=method, frame_mode=mode, suite=suite,
               cap_fps=cap, display_hz=hz, resolution=resolution, status="ok", fps_avg="60", fps_1pct_low="50")
    return row


def test_methodology_states_each_platforms_timing_method_from_the_rows():
    rows = [_method_row("visr", "VISR", "Mac", "M6 Mac mini", "metalperftrace on-glass intervals", "uncapped"),
            _method_row("visr", "VISR", "iPhone/iPad", "M1 iPad Pro", "Instruments Metal System Trace displayed-surfaces-interval", "capped"),
            _method_row("master-chef", "master-chef", "Vision Pro", "M2 Vision Pro", "app telemetry, engine frames per second",
                        "capped", cap="30", hz="90"),
            _method_row("halo-og", "Halo OG", "Mac", "M6 Mac mini", "metalperftrace on-glass intervals", "capped",
                        resolution="3200×2400")]
    text = "\n".join(page.methodology({"records": []}, rows))
    assert "**iPhone/iPad** (M1 iPad Pro; frame cap on): Instruments Metal System Trace" in text
    assert "within 1%" in text
    assert "**Vision Pro** (M2 Vision Pro; frame cap on): The app's own benchmark report" in text
    assert "master-chef runs at a fixed 30 fps as shipped, below the 90 Hz display" in text
    assert "iPhone/iPad and Vision Pro charts use runs presented at the device's display rate" in text
    assert "3200×2400 drawable" in text and "shadow size, model LOD and field of view" in text
    assert "Multiplayer:** a hosted Slayer match on Chill Out and one on Blood Gulch" in text
    assert "No measurements yet" in text.split("**Multiplayer:**")[1].split("**Co-op:**")[0]


def test_multiplayer_line_reports_measured_ports_when_the_data_has_rows():
    rows = [_method_row("visr", "VISR", "Mac", "M6 Mac mini", "metalperftrace on-glass intervals", "uncapped", suite="multiplayer")]
    text = "\n".join(page.methodology({"records": []}, rows))
    assert "Measured on 1 port: VISR." in text.split("**Multiplayer:**")[1].split("**Co-op:**")[0]


def test_device_chart_titles_name_the_display_rate_not_uncapped():
    spec = {"chart": {"title_unit": "fps at display rate"}}
    assert charts.chart_title(spec, {"box": "M1 iPad Pro"}, "campaign") == "Campaign: fps at display rate, M1 iPad Pro"
    assert charts.chart_title({"chart": {}}, {"box": "M6 Mac mini"}, "campaign") == "Campaign: uncapped fps, M6 Mac mini"


def test_a_build_without_a_1pct_low_keeps_the_series_for_the_builds_that_have_one():
    base = {**_method_row("master-chef", "master-chef", "Vision Pro", "M2 Vision Pro", "app telemetry", "capped", cap="30", hz="90"),
            "reference_box": "true", "suite": "campaign", "window": "a10", "date": "2026-10-27", "quality_preset": "defaults"}
    chef = {**base, "label": "master-chef 1.0.3", "version": "1.0.3", "build": "103", "fps_avg": "29.87", "fps_1pct_low": ""}
    visr = {**base, "port": "visr", "port_name": "VISR", "label": "VISR 0.6.0", "version": "0.6.0", "build": "7",
            "frame_mode": "display", "fps_avg": "89.18", "fps_1pct_low": "72.77", "p95_ms": "12.01", "on_time_pct": "91.98"}
    query = {"platform": "Vision Pro", "metrics": ["fps_avg", "fps_1pct_low"], "port_order": ["visr", "master-chef"],
             "details": ["p95_ms", "on_time_pct"],
             "filter": {"frame_mode": ["capped", "display"], "quality_preset": "defaults", "status": "ok"}}
    spec = {"chart": {"horizontal": True}, "series_labels": ["Average fps", "1% low fps"], "query": query, "heading": "Vision Pro",
            "paragraph": "x", "empty": "none"}
    built = charts.model([chef, visr], query, "campaign")
    by_port = {e["port"]: e["value"] for e in built["entries"]}
    assert by_port["master-chef"]["low"] is None and by_port["visr"]["low"] == pytest.approx(72.77)
    text = charts.source(spec, built, "campaign")
    assert "bar [89.2, 29.9]" in text and "bar [72.8, 0]" in text
    assert "1% low" in charts.legend({**spec, "chart": {"marks": ["a", "b"]}}, built)
    note = charts.caption(built, None, query["details"])
    assert "12 ms 95th-percentile frame time, 92% of frames on time" in note and "no frame times reported" in note
    assert "bar [29.9]" in charts.source(spec, built, "campaign", {"master-chef@1.0.3@103"})
    only_chef = charts.model([chef], query, "campaign")
    assert "1% low" not in charts.legend({**spec, "chart": {"marks": ["a", "b"]}}, only_chef)


def test_release_urls_name_each_ports_release_page_and_never_a_download():
    cases = [("visr", "0.6.0", "", "https://github.com/steverice/visr/releases/tag/visr-v0.6.0"),
             ("beeman", "0.3.0", "", "https://github.com/bmanturner/halo-ce-universal/releases/tag/apple-v0.3.0"),
             ("corece", "0.3.0", "44", "https://github.com/mastercheese-dev/CoreCE/releases/tag/build-44"),
             ("snailbbq", "0.2.118-beta", "", "https://github.com/SnailBBQ/Halo-CE-IOS/releases/tag/v0.2.118-beta"),
             ("halopad", "0.3.12", "", "https://github.com/chrissotraidis/projectreach/releases/tag/v0.3.12"),
             ("opence-web", "build-280", "280", "https://halocombatevolved.com/")]
    for port, version, build, expected in cases:
        assert sheet_export.release_url(port, version, build) == expected
    assert sheet_export.release_url("unknown-port", "1", "") == ""
    assert sheet_export.release_url("visr", "main", "") == ""  # a pre-release build has no release page
    assert sheet_export.release_url("corece", "0.3.0", "") == ""  # its release page is named by build
    assert all("/download" not in url and "/assets/" not in url for url in sheet_export.RELEASE_PAGES.values())


def test_a_builds_label_links_to_its_release_in_the_caption_and_the_builds_list():
    rows = charts.load_snapshot(ROOT / "docs/benchmarks/results.csv")
    spec = next(item for item in charts.load_specs(ROOT / "docs/benchmarks/charts") if item["query"]["platform"] == "Apple TV")
    built = charts.model(rows, spec["query"], "campaign")
    assert built["entries"][0]["url"] == "https://github.com/steverice/visr/releases/tag/visr-v0.6.0"
    assert "[VISR 0.6.0](https://github.com/steverice/visr/releases/tag/visr-v0.6.0) (build" in charts.caption(built, None, None, "markdown")
    assert charts.caption(built) == "".join(text for text, _ in charts.caption_parts(built))
    html = site_page.section(spec, rows, "campaign", charts.load_reasons())
    assert 'href="https://github.com/steverice/visr/releases/tag/visr-v0.6.0"' in html


def test_gpu_columns_average_the_rounds_and_stay_empty_unless_every_round_has_them():
    rec = box_record()
    for index, item in enumerate(rec["runs"]):
        item["metrics"].update(gpu_p50_ms=0.4 + 0.1 * (index % 3), gpu_p90_ms=1.0)
    [first, *_] = [r for r in sheet_export.rows(summary(rec)) if r["window"] == "a10"]
    assert (first["gpu_p50_ms"], first["gpu_p90_ms"]) == ("0.50", "1.00")
    del rec["runs"][0]["metrics"]["gpu_p50_ms"]
    [first, *_] = [r for r in sheet_export.rows(summary(rec)) if r["window"] == "a10"]
    assert first["gpu_p50_ms"] == "" and first["gpu_p90_ms"] == "1.00"


def test_uncapped_charts_draw_the_average_alone_and_list_gpu_frame_time_under_the_chart():
    rows = snapshot_rows()
    for row in rows:
        if row["port"] == "visr" and row["version"] == "0.5.1":
            row.update(gpu_p50_ms="0.45", gpu_p90_ms="1.2")
    query = {**QUERY, "metrics": ["fps_avg"], "details": ["gpu_p50_ms", "gpu_p90_ms"]}
    built = charts.model(rows, query, "campaign")
    visr = next(e for e in built["entries"] if e["port"] == "visr" and e["version"] == "0.5.1")
    assert visr["value"]["low"] is None
    note = charts.caption(built, {charts.entry_key(visr)}, query["details"])
    assert "0.45 ms median GPU time per rendered frame, 1.2 ms 90th-percentile GPU time per rendered frame" in note
    other = next(e for e in built["entries"] if e["port"] == "opence-macos")
    assert "no GPU times reported" in charts.caption(built, {charts.entry_key(other)}, query["details"])
    spec = {**QUERY_SPEC, "query": query} if "QUERY_SPEC" in globals() else None


def test_capped_charts_keep_the_one_percent_low_and_list_on_time_share():
    query = {**QUERY, "details": ["on_time_pct"]}
    rows = snapshot_rows()
    for row in rows:
        row.update(on_time_pct="93.26")
    built = charts.model(rows, query, "campaign")
    entry = built["entries"][0]
    assert entry["value"]["low"] is not None
    assert "93.3% of frames on time" in charts.caption(built, {charts.entry_key(entry)}, ["on_time_pct"])


def test_a_one_percent_low_above_the_average_is_drawn_at_the_average():
    rows = snapshot_rows()
    for row in rows:
        row.update(fps_1pct_low="900")
    built = charts.model(rows, QUERY, "campaign")
    entry = built["entries"][0]
    text = charts.source(spec(), built, "campaign", {charts.entry_key(entry)})
    average = text.split("    bar [")[1].split("]")[0]
    low = text.split("    bar [")[2].split("]")[0]
    assert low == average


def test_last_measured_is_the_newest_date_among_the_summary_and_every_row():
    from perf_compare.page import last_measured
    rows = snapshot_rows()
    assert last_measured({"records": [{"date": "2026-10-26"}]}, []) == "2026-10-26"
    assert last_measured({"records": [{"date": "2026-10-26"}]}, [{**rows[0], "date": "2026-10-27"}]) == "2026-10-27"
    assert last_measured({}, [{**rows[0], "date": "2026-10-24"}, {**rows[0], "date": "2026-10-25"}]) == "2026-10-25"
    assert last_measured({}, []) == "unknown"


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_browser_overall_matches_python_on_mixed_suites():
    extra = [{**box_record(fps=(100.0, 100.0), p99=10.0), "suite": "multiplayer"},
             {**box_record(fps=(300.0, 300.0), p99=20.0), "suite": "combat"},
             {**box_record(port="opence-macos", version="v1.0.2", build="2", fps=(50.0, 50.0), p99=40.0), "suite": "combat"}]
    rows = snapshot_rows(*extra)
    script = """
const fs = require('fs');
const m = require(process.argv[1] + '/docs/site/benchmarks/benchmarks.js');
const input = JSON.parse(fs.readFileSync(0, 'utf8'));
const built = m.model(input.rows, input.spec.query, 'overall');
console.log(JSON.stringify(input.selections.map((keys) => {
  const picked = keys === null ? null : new Set(keys);
  return [m.chartSource(input.spec, built, 'overall', picked), m.captionText(built, picked, [])];
})));
"""
    selections = [None, ["visr@0.5.1@7"], ["visr@0.5.1@7", "opence-macos@v1.0.2@2"], []]
    done = subprocess.run(["node", "-e", script, str(ROOT)], capture_output=True, text=True, check=True,
                          input=json.dumps({"rows": rows, "spec": spec(), "selections": selections}))
    built = charts.model(rows, QUERY, "overall")
    expected = [[charts.source(spec(), built, "overall", None if keys is None else set(keys)),
                 charts.caption(built, None if keys is None else set(keys), [])] for keys in selections]
    assert json.loads(done.stdout) == expected
    assert "Overall covers Campaign and Combat." in expected[0][1]
    assert "Overall covers Campaign, Combat and Multiplayer." in expected[1][1]


def window_record(key, runs_extra=None, **extra):
    rec = box_record(**extra)
    rec["scenarios"] = [key]
    rec["runs"] = [{**run(n, key, 200.0 + n, 5.0, "Mac mini (M6, 24 GB)"), "drawable": "2560×1920", **(runs_extra or {})}
                   for n in (1, 2, 3)]
    return rec


def test_new_windows_get_their_suite_kind_drawable_and_fingerprint_columns():
    fingerprint = {"kind": "battle", "end": {"(ai_living_count ext_a_area_a_cov)": "27"}}
    rec = window_record("b40-exta", {"fingerprint": fingerprint, "cpu_p50_ms": 6.1})
    [row] = sheet_export.rows(summary(rec))
    assert (row["suite"], row["window"], row["window_kind"], row["drawable"]) == ("combat", "b40-exta", "battle", "2560×1920")
    assert row["cpu_p50_ms"] == "6.10"
    assert json.loads(row["fingerprint"]) == {"(ai_living_count ext_a_area_a_cov)": ["27", "27", "27"]}
    [cut] = sheet_export.rows(summary(window_record("x70-bridge")))
    assert (cut["suite"], cut["window_kind"]) == ("cutscenes", "cutscene")
    [old] = sheet_export.rows(summary(record()))
    assert (old["suite"], old["window_kind"], old["drawable"], old["cpu_p50_ms"]) == ("campaign", "opening", "", "")


def test_a_window_without_a_console_is_a_not_measured_row_with_its_reason():
    rec = window_record("x70-bridge", port="corece", version="0.3.0", window_status={
        "x70-bridge": {"status": "not_measured", "reason": "no unattended console"}})
    rec["runs"], rec["status"] = [], "unavailable"
    [row] = sheet_export.rows(summary(rec))
    assert (row["status"], row["notes"], row["suite"]) == ("not_measured", "no unattended console", "cutscenes")
    built = charts.model(snapshot_rows() + csv_rows([row]), QUERY, "cutscenes")
    [entry] = [e for e in built["entries"] if e["port"] == "corece"]
    assert (entry["kind"], entry["reason"]) == ("not_measured", "no unattended console")
    assert charts.plain_reason(entry["reason"], charts.load_reasons()) != entry["reason"]


def test_outlier_flags_reach_the_notes_column():
    rec = window_record("a50-hangar")
    rec["runs"][2]["flags"] = ["(ai_living_count hangar_marines) ended at 0, outside the other rounds (3 to 4)"]
    [row] = sheet_export.rows(summary(rec))
    assert row["notes"].startswith("round 3: (ai_living_count hangar_marines)")


def test_fetch_accepts_a_sheet_from_before_the_new_columns():
    old = [c for c in sheet_export.COLUMNS if c not in sheet_export.OPTIONAL]
    text = sheet_export.to_csv(old, [{k: v for k, v in r.items() if k in old} for r in snapshot_rows()])
    cleaned = list(csv.DictReader(io.StringIO(sheet_fetch.validate(text))))
    assert list(cleaned[0]) == list(sheet_export.COLUMNS) and cleaned[0]["drawable"] == ""
    with pytest.raises(ValueError, match="lacks results columns: window"):
        sheet_fetch.validate(sheet_export.to_csv([c for c in old if c != "window"], [{k: "" for k in old if k != "window"}]))


def test_methodology_describes_combat_and_cutscenes_only_once_they_have_rows():
    from perf_compare import page
    plain = "\n".join(page.suite_section(snapshot_rows()))
    assert "**Combat:**" not in plain and "the opening of a10" in plain
    rows = snapshot_rows() + csv_rows(sheet_export.rows(summary(window_record("a50-hangar"), window_record("c10"))))
    text = "\n".join(page.suite_section(rows))
    assert "**Combat:**" in text and "**Cutscenes:**" in text and "the openings of c10" in text
    assert "2560×1920 drawable" in text and "equivalent workloads, not replays" in text
    assert "Overall:" in text and "every build drawn in it has measured" in text
    assert "TestFlight" not in text
