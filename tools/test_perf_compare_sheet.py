"""Sheet export, snapshot fetch, chart generation and page composition."""

from copy import deepcopy
import csv
import io
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from perf_compare import charts, page, sheet_export, sheet_fetch

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


def test_sweep_rows_split_the_reported_range():
    out = sheet_export.sweep_rows(summary(sweep=[{"device": "iPad", "fps": "107.65–109.02", "trials": 3},
                                                 {"device": "Apple TV HD", "fps": "20.27"}]))
    assert {row["device"]: (row["fps_min"], row["fps_max"]) for row in out} == {
        "iPad": ("107.65", "109.02"), "Apple TV HD": ("20.27", "20.27")}


def test_layout_filters_each_port_over_the_results_tab():
    layout = sheet_export.layout(summary(record(), record(port="corece", version="0.3.0")))
    tabs = {tab["name"]: tab for tab in layout["tabs"]}
    assert list(tabs)[:2] == ["README", "results"]
    assert tabs["visr"]["formula"] .startswith("={results!A1:A")
    assert "corece" in tabs and "device-sweep" not in tabs
    last = sheet_export._column_letter(len(sheet_export.COLUMNS) - 1)
    assert f"results!A1:{last}1" in tabs["visr"]["formula"]


def test_summary_schema_is_checked():
    with pytest.raises(ValueError):
        sheet_export.rows({"schema": 1})


def snapshot_rows():
    rows = sheet_export.rows(summary(
        record(), record(port="opence-macos", version="v1.0.2", date="2026-10-24"),
        record(port="opence-macos", version="v1.0.2", date="2026-10-26"), record(version="main", date="2026-10-24")))
    return rows


def test_fetch_accepts_the_results_header_and_normalizes_line_ends():
    text = sheet_export.to_csv(sheet_export.COLUMNS, snapshot_rows()).replace("\n", "\r\n")
    assert "\r" not in sheet_fetch.validate("﻿" + text)


@pytest.mark.parametrize("text", ["<!DOCTYPE html><html>sign in", "a,b\n1,2\n", ",".join(sheet_export.COLUMNS) + "\n"])
def test_fetch_rejects_pages_wrong_tabs_and_empty_results(text):
    with pytest.raises(ValueError):
        sheet_fetch.validate(text)


def test_export_url_names_the_tab():
    assert sheet_fetch.export_url("ID", "12") == "https://docs.google.com/spreadsheets/d/ID/export?format=csv&gid=12"


def spec(**overrides):
    base = {"name": "t", "heading": "H", "paragraph": "P.",
            "chart": {"type": "bar", "title": "T", "x_label": "X", "y_label": "Y", "marks": ["A", "B"],
                      "header": "---\nconfig: {}\n---"},
            "query": {"metric": "fps_avg", "x": ["window"], "series": "port_name", "order_series": ["VISR", "OpenCE-macOS"],
                      "filter": {"host": "M4 Mac mini", "frame_mode": "capped"}, "exclude": {"version": ["main"]}}}
    base.update(overrides)
    return base


def test_chart_keeps_the_newest_set_and_excludes_filtered_versions():
    built = charts.build(spec(), snapshot_rows())
    assert built["source"].startswith("---\nconfig: {}\n---\nxychart-beta")
    assert 'x-axis "X" ["a10 VISR", "a10 OpenCE-macOS"]' in built["source"]
    assert "bar [60, 0]" in built["source"] and "bar [0, 60]" in built["source"]
    assert built["legend"] == "A VISR B OpenCE-macOS"
    assert "VISR 0.5.1 (build 7, source `abc123`)" in built["caption"]
    assert built["caption"].endswith("Measured 2026-10-26.")


def test_single_series_lines_and_renames_need_no_legend():
    query = {"metric": "p99_ms", "x": ["port_name"], "filter": {"host": "M4 Mac mini", "frame_mode": "capped"},
             "exclude": {"version": ["main"]}}
    built = charts.build(spec(chart={"type": "line", "title": "T"}, query=query, rename={"OpenCE-macOS": "OpenCE"}),
                         snapshot_rows())
    assert '["OpenCE", "VISR"]' in built["source"] and "    line [19, 19]" in built["source"]
    assert built["legend"] == ""


def test_a_query_that_matches_nothing_fails_loudly():
    with pytest.raises(ValueError, match="no rows match"):
        charts.build(spec(query={"metric": "fps_avg", "x": ["window"], "filter": {"host": "M9 Mac"}}), snapshot_rows())


def test_a_line_series_with_a_gap_is_rejected():
    query = {"metric": "fps_avg", "x": ["window"], "series": "port_name", "filter": {"host": "M4 Mac mini"}}
    rows = [row for row in snapshot_rows() if row["port"] == "visr"]
    other = deepcopy(rows[0])
    other.update(port_name="CoreCE", window="b30", label="CoreCE 1")
    with pytest.raises(ValueError, match="missing an x value"):
        charts.build(spec(chart={"type": "line", "title": "T"}, query=query), [*rows, other])


def test_y_axis_ceiling_gives_headroom_and_spec_can_pin_it():
    assert charts._nice_ceiling(64.8) == 80
    built = charts.build(spec(chart={**spec()["chart"], "y_max": 100}), snapshot_rows())
    assert 'y-axis "Y" 0 --> 100' in built["source"]


def test_apply_replaces_only_the_marked_block_and_is_idempotent():
    text = f"top\n{charts.BEGIN}\nold\n{charts.END}\nbottom\n"
    once = charts.apply(text, "new")
    assert once.startswith("top\n") and once.endswith("bottom\n") and "old" not in once and "new" in once
    assert charts.apply(once, "new") == once
    with pytest.raises(ValueError):
        charts.apply("no markers", "new")


def test_page_has_charts_first_methodology_last_and_no_result_tables():
    text = page.render_page(summary(record(), record(port="opence-macos", version="v1.0.2")), snapshot_rows(), [spec()])
    assert text.index("## Results") < text.index("```mermaid") < text.index("## Methodology")
    assert page.SHEET_URL in text and "—" not in text
    assert not any(line.startswith("| ") for line in text.splitlines())
    assert text == page.render_page(summary(record(), record(port="opence-macos", version="v1.0.2")), snapshot_rows(), [spec()])


def test_committed_snapshot_has_the_export_columns_and_every_spec_builds_from_it():
    rows = charts.load_snapshot(ROOT / "docs/benchmarks/results.csv")
    assert rows and list(rows[0]) == list(sheet_export.COLUMNS)
    for item in charts.load_specs(ROOT / "docs/benchmarks/charts"):
        assert charts.render_section(item, rows).startswith("### ")


def test_committed_summary_exports_to_the_committed_snapshot_columns():
    committed = json.loads((ROOT / "tools/perf_compare/summary.json").read_text())
    assert [list(row) for row in sheet_export.rows(committed)][0] == list(sheet_export.COLUMNS)


def test_platform_and_reference_box_columns():
    box = record(machine="Mac mini (M6, 24 GB)")
    for item in box["runs"]:
        item["machine"] = "Mac mini (M6, 24 GB)"
    rows = {row["host"]: row for row in sheet_export.rows(summary(record(), box))}
    assert (rows["M6 Mac mini"]["platform"], rows["M6 Mac mini"]["reference_box"]) == ("Mac", "true")
    assert rows["M4 Mac mini"]["reference_box"] == "false"
    assert [sheet_export.platform_of(m) for m in ("iPad Pro", "iPhone 17 Pro", "Apple TV 4K", "Apple Vision Pro")] == [
        "iPhone/iPad", "iPhone/iPad", "Apple TV", "Vision Pro"]


def test_one_percent_low_is_the_rate_of_the_99th_percentile_interval():
    [row] = sheet_export.rows(summary(record()))
    assert row["fps_1pct_low"] == "52.63"  # 1000 / 19 ms


def headline_spec(**query):
    return {"name": "h", "heading": "H", "paragraph": "P.", "empty": "No results yet.",
            "series_labels": ["Average fps", "1% low fps"],
            "chart": {"type": "bar", "horizontal": True, "title": "T", "marks": ["A", "B"]},
            "query": {"metrics": ["fps_avg", "fps_1pct_low"], "x": ["window", "port_name"],
                      "filter": {"platform": "Mac", "frame_mode": "capped"}, **query}}


def test_headline_chart_draws_average_then_low_in_one_horizontal_chart():
    built = charts.build(headline_spec(), snapshot_rows())
    lines = built["source"].splitlines()
    assert lines[0] == "xychart-beta horizontal"
    assert [line.strip() for line in lines if "bar [" in line] == ["bar [60, 60]", "bar [52.6, 52.6]"]
    assert built["legend"] == "A Average fps B 1% low fps"
    assert "VISR 0.5.1 (build 7, source `abc123`)" in built["caption"]


def test_platform_without_results_shows_its_explanation_not_a_chart():
    text = charts.render_section(headline_spec(filter={"platform": "Vision Pro"}), snapshot_rows())
    assert "No results yet." in text and "```mermaid" not in text
    plain = headline_spec(filter={"platform": "Vision Pro"})
    del plain["empty"]
    with pytest.raises(ValueError):
        charts.render_section(plain, snapshot_rows())
