"""Tests for the signed benchmark catalog consumed by the app."""
import json
import struct
from pathlib import Path

import pytest

import benchmark_catalog
import data_pack


def _float_word(value):
    return f"{struct.unpack('<I', struct.pack('<f', value))[0]:08x}"


def _recording(folder, name="take-b30", *, active=True, scene="b30-warthog-fight", map_name="b30",
               difficulty="hard", lead=0, window=1):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / f"{name}.input").write_text(
        "halo input recording 1\n0 0000 0 0 0 0 0 0 0 0 0 0 0 0\n")
    (folder / f"{name}.route").write_text(
        f"scene = {scene}\nkind = gameplay\nlead = {lead}\nwindow = {window}\nmap = {map_name}\n"
        f"difficulty = {difficulty}\nentered = start\nfirst_tick = 0\nplayers = 1\n")
    action_lines = ["halo action recording 1"]
    seed_lines = ["halo seed recording 1"]
    for tick in range(40):
        x = 1.0 if active else 0.0
        action_lines.append(f"0 {tick} 00000000 00000000 00000000 {_float_word(x)} "
                            f"{_float_word(0.0)} {_float_word(0.0)} 00000000 0000ffff")
        seed_lines.append(f"0 {tick} 00000000 12345678")
    (folder / f"{name}.actions").write_text("\n".join(action_lines) + "\n")
    (folder / f"{name}.seeds").write_text("\n".join(seed_lines) + "\n")


def _cases(*, players=2, gameplay=True, multiplayer=True):
    cases = {
        "menu": {"kind": "menu", "order": 0, "map": "", "difficulty": "normal", "recording": None,
                 "lead": 1, "window": 10},
        "a10-opening": {"kind": "cutscene", "order": 1, "map": "a10", "difficulty": "normal",
                        "recording": None, "lead": 2, "window": 60},
    }
    if multiplayer:
        cases["mp-chillout-proof"] = {"kind": "multiplayer", "order": 3 if gameplay else 2, "map": "chillout",
                                       "difficulty": "normal", "recording": None, "lead": 15, "window": 60,
                                       "simulation": "local_controllers" if players == 4 else "network_players",
                                       "players": players, "views": 1, "game_mode": "slayer", "score_limit": 1000,
                                       "time_limit_minutes": 0}
    if gameplay:
        cases["b30-warthog-fight"] = {"kind": "gameplay", "order": 2, "map": "b30", "difficulty": "hard",
                                       "recording": "take-b30", "lead": 0, "window": 1}
    if multiplayer and players >= 16:
        cases["mp-chillout-proof"]["arena"] = {"columns": 6, "spacing_milliunits": 1500, "distance_milliunits": 3500, "movement_radius_milliunits": 400}
    return cases


def _write_catalog(folder, cases, *, raw=None):
    path = folder / "benchmark-catalog.json"
    path.write_text(raw if raw is not None else json.dumps({"schema": 1, "cases": cases}, sort_keys=True,
                                                           separators=(",", ":")))
    return path


def test_catalog_types_order_and_two_player_proof(tmp_path):
    _recording(tmp_path)
    _write_catalog(tmp_path, _cases(players=2))
    cases = benchmark_catalog.load_catalog(tmp_path)
    assert [case["id"] for case in cases] == ["menu", "a10-opening", "b30-warthog-fight", "mp-chillout-proof"]
    assert cases[-1]["kind"] == "multiplayer" and cases[-1]["players"] == 2


def test_catalog_accepts_four_player_handler_value(tmp_path):
    _recording(tmp_path)
    _write_catalog(tmp_path, _cases(players=4))
    assert next(case for case in benchmark_catalog.load_catalog(tmp_path) if case["kind"] == "multiplayer")["players"] == 4


@pytest.mark.parametrize("players", [2, 3, 16, 32])
def test_catalog_accepts_bounded_network_rosters(tmp_path, players):
    _write_catalog(tmp_path, _cases(players=players, gameplay=False))
    cases = benchmark_catalog.load_catalog(tmp_path)
    assert cases[-1]["players"] == players
    assert cases[-1]["simulation"] == "network_players"


def test_multiplayer_only_catalog_needs_no_campaign_recording(tmp_path):
    _write_catalog(tmp_path, _cases(gameplay=False))
    cases = benchmark_catalog.load_catalog(tmp_path)
    assert len(cases) == 3 and all(case["kind"] != "gameplay" for case in cases)


def test_missing_catalog_fails_closed(tmp_path):
    with pytest.raises(benchmark_catalog.CatalogError, match="catalog"):
        benchmark_catalog.load_catalog(tmp_path)


def test_catalog_requires_active_selected_gameplay_window(tmp_path):
    _recording(tmp_path, active=False)
    _write_catalog(tmp_path, _cases())
    with pytest.raises(benchmark_catalog.CatalogError, match="movement or combat"):
        benchmark_catalog.load_catalog(tmp_path)


def test_catalog_checks_every_selected_action_after_movement(tmp_path):
    _recording(tmp_path)
    actions = tmp_path / "take-b30.actions"
    lines = actions.read_text().splitlines()
    lines[2] = "malformed action after an active tick"
    actions.write_text("\n".join(lines) + "\n")
    _write_catalog(tmp_path, _cases())
    with pytest.raises(benchmark_catalog.CatalogError, match="malformed action"):
        benchmark_catalog.load_catalog(tmp_path)


def test_catalog_accepts_legacy_fresh_start_and_repeated_segment_seeds(tmp_path):
    _recording(tmp_path)
    route = tmp_path / "take-b30.route"
    route_text = route.read_text().replace("first_tick = 0", "first_tick = 1")
    route.write_text(route_text + "segment_seed = 1 10 12345678\nsegment_seed = 2 20 87654321\n")
    _write_catalog(tmp_path, _cases())
    assert any(case["kind"] == "gameplay" for case in benchmark_catalog.load_catalog(tmp_path))


@pytest.mark.parametrize("cases,reason", [
    (_cases(gameplay=False, multiplayer=False), "supported multiplayer workload"),
    (_cases(players=33), "players"),
    (_cases() | {"bad-id!": {"kind": "menu", "order": 4, "map": "", "difficulty": "normal",
                             "recording": None, "lead": 0, "window": 1}}, "ID"),
])
def test_catalog_rejects_unsupported_or_empty_workloads(tmp_path, cases, reason):
    if cases.get("b30-warthog-fight"):
        _recording(tmp_path)
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError, match=reason):
        benchmark_catalog.load_catalog(tmp_path)


def test_catalog_rejects_duplicate_json_keys(tmp_path):
    raw = '{"schema":1,"cases":{"menu":{"kind":"menu"},"menu":{"kind":"cutscene"}}}'
    _write_catalog(tmp_path, {}, raw=raw)
    with pytest.raises(benchmark_catalog.CatalogError, match="duplicate"):
        benchmark_catalog.load_catalog(tmp_path)


def test_catalog_rejects_duplicate_order_and_replay_per_level(tmp_path):
    _recording(tmp_path)
    cases = _cases()
    cases["menu"]["order"] = 1
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError, match="order"):
        benchmark_catalog.load_catalog(tmp_path)

    cases = _cases()
    cases["b30-second-take"] = dict(cases["b30-warthog-fight"], order=4, recording="take-b30-second")
    _recording(tmp_path, name="take-b30-second", scene="b30-second-take")
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError, match="per level"):
        benchmark_catalog.load_catalog(tmp_path)


def test_new_compatibility_accepts_catalog_while_legacy_does_not(tmp_path):
    _recording(tmp_path)
    _write_catalog(tmp_path, _cases())
    assert data_pack.snapshot_name_allowed("benchmark-catalog.json", "recordings",
                                          "benchmark-catalog-1:" + "a" * 64)
    assert not data_pack.snapshot_name_allowed("benchmark-catalog.json", "recordings",
                                               "benchmark-replay-1:" + "a" * 64)
    files = data_pack.snapshot(tmp_path, "recordings", "benchmark-catalog-1:" + "a" * 64)
    assert "benchmark-catalog.json" in files


def test_multiplayer_only_signed_pack_needs_no_placeholder_routes(tmp_path):
    _write_catalog(tmp_path, _cases(gameplay=False))
    files = data_pack.snapshot(tmp_path, "recordings", "benchmark-catalog-1:" + "b" * 64)
    assert set(files) == {"benchmark-catalog.json"}


def test_catalog_requires_signed_arena_for_scaled_rosters(tmp_path):
    cases = _cases(players=32, gameplay=False)
    cases["mp-chillout-proof"].pop("arena")
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError, match="arena"):
        benchmark_catalog.load_catalog(tmp_path)


@pytest.mark.parametrize("field,value", [("columns", 0), ("columns", 9), ("spacing_milliunits", 100),
    ("distance_milliunits", 30000), ("movement_radius_milliunits", 3000), ("movement_radius_milliunits", True)])
def test_catalog_rejects_unsafe_arena_values(tmp_path, field, value):
    cases = _cases(players=32, gameplay=False)
    cases["mp-chillout-proof"]["arena"][field] = value
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError, match="arena"):
        benchmark_catalog.load_catalog(tmp_path)


@pytest.mark.parametrize("radius,accepted", [(300, True), (301, False)])
def test_signed_integer_arena_boundary(tmp_path, radius, accepted):
    cases = _cases(players=32, gameplay=False)
    cases["mp-chillout-proof"]["arena"].update(spacing_milliunits=900, movement_radius_milliunits=radius)
    _write_catalog(tmp_path, cases)
    if accepted:
        assert benchmark_catalog.load_catalog(tmp_path)[-1]["arena"]["movement_radius_milliunits"] == radius
    else:
        with pytest.raises(benchmark_catalog.CatalogError, match="arena"):
            benchmark_catalog.load_catalog(tmp_path)
