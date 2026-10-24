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
        "date": "2026-10-23", "refresh_seconds": 123.5,
        "machine": "Mac mini", "os": "macOS 27",
        "targets": [{"id": "native", "version": "v1", "sha256": "abc",
            "method": "metal-hud", "status": "ok", "reason": "",
            "runs": [{"scenario": "a10", "round": i,
                "metrics": metrics([10.0 * i, 20.0 * i], 60),
                "thermal": "nominal", "resolution": "1280×720", "cap": 60}
                for i in [1, 2, 3]]}],
        "sweep": [{"device": "iPad", "build": "main", "workload": "a10",
            "resolution": "1280×720", "fps": 210, "timing_basis": "offscreen_uncapped",
            "thermal": "nominal", "report": "sweep-1"}],
    }


def test_render_groups_rounds_and_reports_ranges_for_all_metrics():
    output = render(summary_fixture())
    assert output.count("| native | a10 |") == 1
    assert "22.22–66.67" in output
    assert "15.00–45.00" in output
    assert "19.50–58.50" in output
    assert "19.90–59.70" in output
    assert "0.00–50.00" in output
    assert "20.00–60.00" in output
    assert "2–2" in output
    assert "0.03–0.09" in output
    for value in ["2026-10-23", "123.5", "Mac mini", "macOS 27", "v1", "abc", "metal-hud"]:
        assert value in output


def test_render_marks_missing_scenarios_and_duplicate_rounds_incomplete():
    summary = summary_fixture()
    summary["targets"][0]["runs"][2]["round"] = 2
    output = render(summary)
    assert "incomplete (2/3 rounds)" in output
    assert "| native | b30 |" in output
    assert "incomplete (0/3 rounds)" in output


def test_render_preserves_unavailable_targets_and_escapes_cells():
    summary = summary_fixture()
    summary["targets"] = [{"id": "missing|port", "version": "unknown", "sha256": "",
        "method": "metal-hud", "status": "unavailable", "reason": "No HUD\nretry <later>", "runs": []}]
    output = render(summary)
    assert "missing\\|port" in output
    assert "No HUD retry &lt;later&gt;" in output
    assert "unavailable" in output


def test_render_separates_hud_raf_and_offscreen_without_ranking():
    summary = summary_fixture()
    browser = deepcopy(summary["targets"][0])
    browser.update(id="browser", method="safari-raf")
    summary["targets"].append(browser)
    output = render(summary)
    hud = output.index("## Mac native Metal HUD")
    raf = output.index("## Safari requestAnimationFrame")
    sweep = output.index("## VISR offscreen sweep")
    assert hud < output.index("| native | a10 |") < raf
    assert raf < output.index("| browser | a10 |") < sweep
    assert sweep < output.index("| iPad |")
    assert "repeated game images" in output
    assert "unique game frames" in output
    assert "not ranked" in output
    assert output == render(summary)


def test_render_empty_inputs_keep_missing_evidence_visible():
    output = render({})
    assert "No targets recorded" in output
    assert "No sweep results recorded" in output


def test_render_keeps_markdown_cell_content_literal():
    summary = summary_fixture()
    summary["targets"][0]["reason"] = "[link](url) *bold* `code` _emphasis_"
    output = render(summary)
    assert "\\[link\\](url) \\*bold\\* \\`code\\` \\_emphasis\\_" in output


def test_render_sweep_preserves_configuration_identity_and_trial_count():
    summary = summary_fixture()
    summary["sweep"][0].update(configuration="performance", settings_hash="settings-123", trials=3, fps="180–210")
    output = render(summary)
    assert "Configuration" in output
    assert "Settings hash" in output
    assert "Trials" in output
    assert "performance" in output
    assert "settings-123" in output
    assert "| 3 |" in output
    assert "180–210" in output


def test_render_display_rate_and_operator_qualification_for_each_method():
    summary = summary_fixture()
    for run in summary["targets"][0]["runs"]:
        run.update(display_hz=60, display_rate_basis="operator-qualified")
    browser = deepcopy(summary["targets"][0])
    browser.update(id="browser", method="safari-raf")
    for run in browser["runs"]:
        run.update(display_hz=120, display_rate_basis="operator-qualified")
    summary["targets"].append(browser)
    output = render(summary)
    native_row = next(line for line in output.splitlines() if line.startswith("| native | a10 |"))
    browser_row = next(line for line in output.splitlines() if line.startswith("| browser | a10 |"))
    assert "Display (Hz)" in output
    assert "Display rate basis" in output
    assert "| 60 | operator-qualified |" in native_row
    assert "| 120 | operator-qualified |" in browser_row


def test_render_marks_failed_sweep_import_unavailable():
    summary = summary_fixture()
    summary.update(sweep=[], sweep_status="unavailable: invalid export")
    output = render(summary)
    assert "unavailable: invalid export" in output[output.index("## VISR offscreen sweep"):]


def test_render_separates_offscreen_submission_and_unknown_timing_bases():
    summary = summary_fixture()
    submission = dict(summary["sweep"][0], device="iPhone", timing_basis="submission_timing")
    unknown = dict(summary["sweep"][0], device="Mac", timing_basis="unrecognized")
    summary["sweep"].extend([submission, unknown])
    output = render(summary)
    offscreen = output.index("## VISR offscreen sweep")
    submission_section = output.index("## VISR submission timing")
    unknown_section = output.index("## VISR other timing bases")
    assert offscreen < output.index("| iPad |") < submission_section
    assert submission_section < output.index("| iPhone |") < unknown_section
    assert unknown_section < output.index("| Mac |")
    assert "self-reported" in output[submission_section:unknown_section]
    assert "not displayed pacing" in output[submission_section:unknown_section]
