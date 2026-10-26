"""Interval parsing and published benchmark summaries."""

from copy import deepcopy
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from perf_compare.results import metrics, parse_hud, parse_raf, render


def test_hud_uses_interval_column_and_deduplicates_frames():
    text = """unrelated 1,2,3,4,5
metal-HUD: 7,120,240,16.5,3.1
timestamp process metal-HUD: 8,121,241,17,3.2
metal-HUD: 7,120,240,90,3.1
"""
    assert parse_hud(text) == [16.5, 17.0]


def test_hud_ignores_malformed_nonfinite_and_nonpositive_intervals():
    text = "\n".join(f"metal-HUD: {line}" for line in [
        "frame,memory,memory,interval,gpu", "1,2,3", "x,2,3,16,1",
        "1,2,3,nan,1", "2,2,3,inf,1", "3,2,3,0,1",
        "4,2,3,-1,1", "5,broken,3,16,1", "6,2,3,20,1",
    ])
    assert parse_hud(text) == [20.0]


def test_hud_reads_all_pairs_and_deduplicates_overlapping_batches():
    text = """metal-HUD: 10,120,240,16,1,17,2,18,3
metal-HUD: 12,120,240,99,4,19,5,20,6
"""
    assert parse_hud(text) == [16.0, 17.0, 18.0, 19.0, 20.0]


@pytest.mark.parametrize("key", ["eventMessage", "message"])
def test_hud_decodes_ndjson_message_before_reading_last_gpu_field(key):
    text = json.dumps({key: "metal-HUD: 1,120,240,16,0,17,1", "other": "metadata"})
    assert parse_hud(text) == [16.0, 17.0]


def test_hud_rejects_malformed_records_instead_of_reading_partial_batches():
    text = "\n".join([
        "metal-HUD: 1,120,240,16,1,17",
        "metal-HUD: 1,-120,240,16,1",
        "metal-HUD: 1,inf,240,16,1",
        '{"eventMessage": "metal-HUD: 1,120,240,16,1"',
        json.dumps({"eventMessage": 4, "other": "metal-HUD: 1,120,240,16,1"}),
    ])
    assert parse_hud(text) == []


def test_hud_skips_invalid_pairs_without_shifting_later_frame_numbers():
    text = """metal-HUD: 10,120,240,16,1,nan,2,18,0,19,inf,20,-1,21,bad
metal-HUD: 11,120,240,17,1,99,1
metal-HUD: 16,120,240,22,1
"""
    assert parse_hud(text) == [16.0, 18.0, 17.0, 22.0]


def test_raf_differences_are_milliseconds():
    assert parse_raf([100.0, 116.5, 134.0]) == [16.5, 17.5]
    assert parse_raf([]) == []
    assert parse_raf([100.0]) == []


@pytest.mark.parametrize("timestamps", [[0, 0], [2, 1], [0, float("nan")], [float("inf")]])
def test_raf_rejects_invalid_timestamp_sequences(timestamps):
    with pytest.raises(ValueError):
        parse_raf(timestamps)


def test_metrics_use_duration_weighted_fps_and_interpolated_percentiles():
    result = metrics([10.0, 20.0, 30.0, 40.0], 60)
    assert result == pytest.approx({
        "fps": 40.0, "p50_ms": 25.0, "p95_ms": 38.5,
        "p99_ms": 39.7, "on_time_pct": 25.0, "worst_ms": 40.0,
        "frames": 4, "duration_s": 0.1,
    })


def test_metrics_include_five_percent_on_time_boundary():
    assert metrics([17.5, 17.50001], 60)["on_time_pct"] == 50


@pytest.mark.parametrize("intervals,hz", [([], 60), ([0], 60), ([-1], 60),
    ([float("nan")], 60), ([float("inf")], 60), ([10], 0), ([10], float("nan"))])
def test_metrics_reject_invalid_samples_or_display_rate(intervals, hz):
    with pytest.raises(ValueError):
        metrics(intervals, hz)


def summary_fixture():
    return {
        "schema": 2,
        "records": [{"port": "native", "version": "v1", "build": "7", "source": "abc1234", "release_kind": "release",
            "sha256": "abc", "machine": "Mac mini (M4, 16 GB)", "os": "macOS 27", "date": "2026-10-23",
            "method": "metal-hud", "status": "ok", "reason": "", "matrix_seconds": 123.5,
            "runs": [{"scenario": "a10", "round": i,
                "metrics": metrics([10.0 * i, 20.0 * i], 60),
                "thermal": "nominal", "resolution": "1280×720", "cap": 60}
                for i in [1, 2, 3]]}],
    }


NATIVE = "| native v1 (build 7, source abc1234) | Mac mini (M4, 16 GB) | 2026-10-23 |"


def test_render_groups_rounds_and_reports_ranges_for_all_metrics():
    output = render(summary_fixture())
    assert output.count(NATIVE + " a10 |") == 1
    assert "22.22–66.67" in output
    assert "15.00–45.00" in output
    assert "19.50–58.50" in output
    assert "19.90–59.70" in output
    assert "0.00–50.00" in output
    assert "20.00–60.00" in output
    assert "2–2" in output
    assert "0.03–0.09" in output
    for value in ["2026-10-23", "123.5", "Mac mini (M4, 16 GB)", "macOS 27", "v1", "abc", "metal-hud"]:
        assert value in output


def test_every_row_names_version_build_source_and_machine():
    output = render(summary_fixture())
    rows = [line for line in output.splitlines() if "| a10 |" in line and "Mac mini" in line]
    assert len(rows) == 2  # side by side and the port's own table
    assert all("native v1 (build 7, source abc1234)" in row and "Mac mini (M4, 16 GB)" in row for row in rows)


def test_versions_of_one_port_coexist_and_group_under_the_port():
    summary = summary_fixture()
    newer = deepcopy(summary["records"][0])
    newer.update(version="v2", build="9", source="def5678", date="2026-10-25")
    summary["records"].append(newer)
    output = render(summary)
    section = output[output.index("## native"):]
    assert "Measured versions: native v1 (build 7, source abc1234); native v2 (build 9, source def5678)." in section
    assert "| native v1 (build 7, source abc1234) |" in section
    assert "| native v2 (build 9, source def5678) |" in section
    assert "Last measured: 2026-10-25." in output


def test_pre_release_builds_are_labeled_in_every_row():
    summary = summary_fixture()
    summary["records"][0].update(version="main", release_kind="pre-release main build")
    output = render(summary)
    rows = [line for line in output.splitlines() if "| a10 |" in line and "Mac mini" in line]
    assert len(rows) == 2 and all("native main (pre-release, build 7, source abc1234)" in row for row in rows)
    assert "not the released product" in output


def test_merge_adds_new_versions_and_never_replaces_a_measured_record():
    from perf_compare.results import merge
    old = summary_fixture()
    new = deepcopy(old)
    new["records"][0].update(version="v2", build="9")
    merged = merge(old, new)
    assert [(r["version"], r["build"]) for r in merged["records"]] == [("v1", "7"), ("v2", "9")]
    assert merge(merged, new) == merged
    changed = deepcopy(new)
    changed["records"][0]["runs"] = changed["records"][0]["runs"][:1]
    with pytest.raises(ValueError, match="already exists"):
        merge(merged, changed)


def test_merge_replaces_only_a_record_without_samples():
    from perf_compare.results import merge
    old = summary_fixture()
    old["records"][0].update(runs=[], status="unavailable", reason="console-unavailable")
    new = summary_fixture()
    merged = merge(old, new)
    assert len(merged["records"]) == 1 and merged["records"][0]["status"] == "ok"


def test_render_rejects_an_unversioned_summary():
    with pytest.raises(ValueError, match="schema"):
        render({"targets": []})


def test_render_marks_missing_scenarios_and_duplicate_rounds_incomplete():
    summary = summary_fixture()
    summary["records"][0]["runs"][2]["round"] = 2
    output = render(summary)
    assert "incomplete (2/3 rounds)" in output
    assert NATIVE + " b30 |" in output
    assert "incomplete (0/3 rounds)" in output


def test_render_preserves_unavailable_targets_and_escapes_cells():
    summary = summary_fixture()
    summary["records"] = [{"port": "missing|port", "version": "unknown", "sha256": "",
        "method": "metal-hud", "status": "unavailable", "reason": "No HUD\nretry <later>", "runs": []}]
    output = render(summary)
    assert "missing\\|port" in output
    assert "No HUD retry &lt;later&gt;" in output
    assert "unavailable" in output


def test_render_separates_hud_and_raf_without_ranking():
    summary = summary_fixture()
    browser = deepcopy(summary["records"][0])
    browser.update(port="browser", method="safari-raf", browser="Safari 27")
    summary["records"].append(browser)
    output = render(summary)
    browser_section = output.index("## browser")
    raf = output.index("Safari callbacks measure")
    native_section = output.index("## native")
    hud = output.index("Metal HUD per-frame intervals")
    assert browser_section < raf < output.rindex("| browser v1 (build 7, source abc1234) |") < native_section
    assert native_section < hud < output.rindex(NATIVE + " a10 |")
    assert "Safari 27" in output
    assert "repeated game images" in output
    assert "unique game frames" in output
    assert "not ranked" in output
    assert output == render(summary)


def test_render_empty_inputs_keep_missing_evidence_visible():
    output = render({})
    assert "No targets recorded" in output


def test_render_keeps_markdown_cell_content_literal():
    summary = summary_fixture()
    summary["records"][0]["reason"] = "[link](url) *bold* `code` _emphasis_"
    output = render(summary)
    assert "\\[link\\](url) \\*bold\\* \\`code\\` \\_emphasis\\_" in output


def test_render_display_rate_and_operator_qualification_for_each_method():
    summary = summary_fixture()
    for run in summary["records"][0]["runs"]:
        run.update(display_hz=60, display_rate_basis="operator-qualified")
    browser = deepcopy(summary["records"][0])
    browser.update(port="browser", method="safari-raf")
    for run in browser["runs"]:
        run.update(display_hz=120, display_rate_basis="operator-qualified")
    summary["records"].append(browser)
    output = render(summary)
    native_row = [line for line in output.splitlines() if line.startswith(NATIVE + " a10 |")][-1]
    browser_row = [line for line in output.splitlines()
                   if line.startswith("| browser v1 (build 7, source abc1234) | Mac mini (M4, 16 GB) | 2026-10-23 | a10 |")][-1]
    assert "Display (Hz)" in output
    assert "Display rate basis" in output
    assert "| 60 | operator-qualified |" in native_row
    assert "| 120 | operator-qualified |" in browser_row


def test_page_opens_with_the_fairness_caveat():
    from perf_compare.results import CAVEAT
    page = render({})
    assert page.splitlines()[2] == CAVEAT
    assert "https://github.com/steverice/visr/issues" in CAVEAT


def test_trace_uses_only_owned_on_glass_intervals_and_deduplicates():
    from perf_compare.results import parse_metal_trace
    layer = {'Layer ID': 7, 'Configuration Timeline': [{'Width (pixels)': 1280, 'Height (pixels)': 960}],
             'On-Glass Drawable Timeline': [
                 {'Start Mach Continuous Time': 10, 'Duration MCT Seconds': 0.016,
                  'In-Flight Drawable Info': {'On-GPU Time (seconds)': 0.001}},
                 {'Start Mach Continuous Time': 20, 'Duration MCT Seconds': 0.017}]}
    trace = [{'PID': 1, 'Layers': [layer]}, {'PID': 2, 'Layers': []}]
    record = {'pid': 1, 'traces': [trace, trace]}
    assert parse_metal_trace(record, '1280×960') == [16, 17]
    with pytest.raises(ValueError, match='resolution-mismatch'):
        parse_metal_trace(record, '1280×720')
    layer['On-Glass Drawable Timeline'][0]['Duration MCT Seconds'] = float('nan')
    with pytest.raises(ValueError, match='invalid on-glass'):
        parse_metal_trace(record)


def test_trace_rejects_ambiguous_layers():
    from perf_compare.results import parse_metal_trace
    frame = {'Start Mach Continuous Time': 1, 'Duration MCT Seconds': 0.016}
    record = {'pid': 1, 'traces': [[{'PID': 1, 'Layers': [
        {'Layer ID': i, 'On-Glass Drawable Timeline': [frame]} for i in (1, 2)]}]]}
    with pytest.raises(ValueError, match='multiple presentation layers'):
        parse_metal_trace(record)


def test_render_keeps_capped_uncapped_and_later_windows_distinct():
    summary = summary_fixture()
    target = summary['records'][0]
    target.update(frame_mode='uncapped', native_delay_seconds=90)
    other = dict(target, frame_mode='capped', native_delay_seconds=0)
    summary['records'].append(other)
    output = render(summary)
    assert 'uncapped; +90 s' in output
    assert 'capped; +0 s' in output
    assert 'max_fps = -1' in output and 'vsync = false' in output


def test_uncapped_rows_disclose_unverified_above_refresh_meaning():
    summary = summary_fixture()
    summary['records'][0].update(frame_mode='uncapped', quality_preset='matched-stock')
    output = render(summary)
    assert 'Above-refresh trace rates have unverified physical meaning' in output
    assert 'not verified submission throughput' in output
    assert '128-pixel shadow maps' in output
