"""tools/replay_check.py's drop-in route: the unsigned local catalog it writes passes the app's catalog rules."""
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchmark_catalog  # noqa: E402
import replay_check  # noqa: E402

ROUTE = """map = b30
difficulty = normal
entered = start
first_tick = 0
random_seed = 23df5293
players = 1
last_tick = 150
seconds = 5.0
"""


def take(folder):
    base = folder / "take-b30-20261020-000000"
    moving = struct.unpack("<I", struct.pack("<f", 0.5))[0]
    actions = ["halo action recording 1"] + [f"0 {tick} 00000000 00000000 00000000 {moving:08x} 00000000 00000000 "
                                             "ffffffff 0000ffff" for tick in range(150)]
    base.with_suffix(".actions").write_text("\n".join(actions) + "\n")
    base.with_suffix(".input").write_text("input\n")
    base.with_suffix(".seeds").write_text("seeds\n")
    base.with_suffix(".route").write_text(ROUTE)
    return base


def test_local_catalog_loads(tmp_path):
    local = tmp_path / "local-routes"
    replay_check.write_local_routes(take(tmp_path), local, "check")
    [case] = benchmark_catalog.load_catalog(local)
    assert (case["id"], case["kind"], case["map"], case["recording"], case["lead"], case["window"]) == \
        ("check", "gameplay", "b30", "check", 0, 4)
    assert "window = 4" in (local / "check.route").read_text()
    assert sorted(path.name for path in local.iterdir()) == [
        "benchmark-catalog.json", "check.actions", "check.input", "check.route", "check.seeds"]


def test_local_catalog_repeats_the_takes_enemy_multiplier(tmp_path):
    folder = take(tmp_path)
    folder.with_suffix(".route").write_text(ROUTE + "enemy_multiplier = 2.000\n")
    local = tmp_path / "local-routes"
    replay_check.write_local_routes(folder, local, "check")
    [case] = benchmark_catalog.load_catalog(local)
    assert case["enemy_multiplier"] == 2.0
    assert "enemy_multiplier = 2.000" in (local / "check.route").read_text()


def test_local_catalog_compiled_out_without_routes():
    source = (Path(replay_check.ROOT) / "port/ios/host/host_benchmark.m").read_text()
    start = source.index("static NSString *local_catalog_folder(void) {")
    body = source[start:source.index("\n}\n", start)]
    assert "#if HALO_BENCHMARK_NO_ROUTES\n    return nil;\n#else" in body
    assert 'updates[@"benchmark_catalog_status"] = @"local_unsigned";' in source
    assert 'record[@"catalog_signed"] = @(catalog_signed());' in source


def test_suite_launch_keeps_local_routes(tmp_path):
    import mac_run

    folder = tmp_path / "benchmark"
    (folder / "local-routes").mkdir(parents=True)
    (folder / "local-routes" / "benchmark-catalog.json").write_text("{}")
    (folder / "recordings").mkdir()
    (folder / "journal.txt").write_text("old")
    mac_run.clear_benchmark_folder(folder)
    assert [path.name for path in folder.iterdir()] == ["local-routes"]
    assert (folder / "local-routes" / "benchmark-catalog.json").is_file()
