"""tools/benchmark_routes.py: replays' routes compared, and a recording's own route written only when they agree."""
import json

from tools import benchmark_routes


def note(points):
    return ";".join(f"{tick:.0f}:{x:.2f},{y:.2f},{z:.2f}" for tick, x, y, z in points)


def run_file(folder, name, recording, points, status="ok", index=0):
    path = folder / name
    path.write_text(json.dumps({"index": index, "recording": recording, "status": status, "note": note(points)}))
    return path


STRAIGHT = [(300.0 * step, 1.0 * step, 2.0, 0.5) for step in range(1, 9)]


def test_route_points_reads_a_note_and_refuses_other_text():
    assert benchmark_routes.route_points("300:1.00,2.00,0.50;601:2.00,2.00,0.50") == [(300.0, 1.0, 2.0, 0.5), (601.0, 2.0, 2.0, 0.5)]
    assert benchmark_routes.route_points("the map did not start") == []
    assert benchmark_routes.route_points("") == [] and benchmark_routes.route_points(None) == []


def test_replays_a_tick_apart_agree_and_a_replay_that_leaves_is_found():
    late = [(tick + 1, x + 0.2, y, z) for tick, x, y, z in STRAIGHT]
    assert benchmark_routes.compare_routes(STRAIGHT, late)[2] is None
    left = [(tick, x, y + (6.0 if tick >= 1500 else 0.0), z) for tick, x, y, z in STRAIGHT]
    compared, worst, parted = benchmark_routes.compare_routes(STRAIGHT, left)
    assert compared == 8 and worst == 6.0 and parted == 1500.0


def test_checks_are_written_only_when_enough_replays_agree(tmp_path):
    inputs = [run_file(tmp_path, f"run-{n}.json", "a10-bench", STRAIGHT) for n in range(3)]
    out = tmp_path / "recordings"
    out.mkdir()
    assert benchmark_routes.main(["checks", *map(str, inputs), "--out", str(out)]) == 0
    assert benchmark_routes.route_points((out / "a10-bench.checks").read_text().strip()) == STRAIGHT
    # two replays are not enough
    (out / "a10-bench.checks").unlink()
    assert benchmark_routes.main(["checks", *map(str, inputs[:2]), "--out", str(out)]) == 1
    assert not (out / "a10-bench.checks").exists()
    # nor are three when one left the route, or ended early
    left = run_file(tmp_path, "left.json", "a10-bench", [(t, x, y + 9.0, z) for t, x, y, z in STRAIGHT])
    assert benchmark_routes.main(["checks", str(inputs[0]), str(inputs[1]), str(left), "--out", str(out)]) == 1
    short = run_file(tmp_path, "short.json", "a10-bench", STRAIGHT[:4])
    assert benchmark_routes.main(["checks", str(inputs[0]), str(inputs[1]), str(short), "--out", str(out)]) == 1
    assert not (out / "a10-bench.checks").exists()
    assert benchmark_routes.main(["compare", *map(str, inputs)]) == 0
    assert benchmark_routes.main(["compare", str(inputs[0]), str(left)]) == 1


def test_a_report_s_runs_are_read_and_runs_without_a_recording_are_left_out(tmp_path):
    report = tmp_path / "report.json"
    report.write_text(json.dumps({"runs": [
        {"index": 0, "recording": None, "status": "ok", "note": ""},
        {"index": 1, "recording": "c40-bench", "status": "ok", "note": note(STRAIGHT)}]}))
    found = benchmark_routes.by_recording([report])
    assert list(found) == ["c40-bench"] and found["c40-bench"][0][2] == STRAIGHT
