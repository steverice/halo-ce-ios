"""Tests for trimming a benchmark catalog's recordings to their windows."""
import json
from pathlib import Path

import benchmark_catalog
import trim_recordings
from test_benchmark_catalog import _recording


def _long_recording(folder, ticks, **kwargs):
    _recording(folder, name="b30", scene="b30", map_name="b30", **kwargs)
    actions = (folder / "b30.actions").read_text().splitlines()
    seeds = (folder / "b30.seeds").read_text().splitlines()
    for tick in range(40, ticks):
        actions.append(f"0 {tick} " + actions[1].split(" ", 2)[2])
        seeds.append(f"0 {tick} 00000000 12345678")
    (folder / "b30.actions").write_text("\n".join(actions) + "\n")
    (folder / "b30.seeds").write_text("\n".join(seeds) + "\n")
    (folder / "b30.input").write_text("halo input recording 1\n" + "".join(
        f"{tick}.0000 0000 0 0 0 0 0 0 0 0 0 0 0 0\n" for tick in range(0, ticks, 10)))
    catalog = {"schema": 1, "cases": {"b30": {"kind": "gameplay", "order": 0, "map": "b30", "difficulty": "hard",
                                              "recording": "b30", "lead": 0, "window": 1}}}
    (folder / "benchmark-catalog.json").write_bytes(benchmark_catalog._canonical(catalog))


def test_tail_beyond_the_window_is_dropped_and_the_result_loads(tmp_path):
    source, output = tmp_path / "source", tmp_path / "output"
    _long_recording(source, 3000)
    done = trim_recordings.trim_catalog(source, output)
    keep = trim_recordings.window_end_tick({"lead": 0, "window": 1}, {"events": ""})
    assert done == {"b30": keep} and keep < 3000
    assert len((output / "b30.actions").read_text().splitlines()) == keep + 1
    assert len((output / "b30.seeds").read_text().splitlines()) == keep + 1
    # the head is byte-identical, so the replay up to the window sees the same inputs
    full = (source / "b30.actions").read_text().splitlines()
    assert (output / "b30.actions").read_text().splitlines() == full[:keep + 1]
    assert (output / "benchmark-catalog.json").read_bytes() == (source / "benchmark-catalog.json").read_bytes()
    assert (output / "b30.route").read_bytes() == (source / "b30.route").read_bytes()
    assert benchmark_catalog.load_catalog(output)


def test_short_recording_is_left_whole(tmp_path):
    source, output = tmp_path / "source", tmp_path / "output"
    _long_recording(source, 100)
    trim_recordings.trim_catalog(source, output)
    for name in ("actions", "seeds", "input"):
        assert (output / f"b30.{name}").read_bytes() == (source / f"b30.{name}").read_bytes()


def test_anchor_moves_the_window_end(tmp_path):
    case = {"lead": 2, "window": 10, "anchor": "checkpoint:1"}
    anchored = trim_recordings.window_end_tick(case, {"events": "checkpoint:1:900"})
    plain = trim_recordings.window_end_tick({"lead": 2, "window": 10}, {"events": ""})
    assert anchored - plain == 900
