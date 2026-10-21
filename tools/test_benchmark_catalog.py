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
               difficulty="hard", lead=0, window=1, route_extra=""):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / f"{name}.input").write_text(
        "halo input recording 1\n0 0000 0 0 0 0 0 0 0 0 0 0 0 0\n")
    (folder / f"{name}.route").write_text(
        f"scene = {scene}\nkind = gameplay\nlead = {lead}\nwindow = {window}\nmap = {map_name}\n"
        f"difficulty = {difficulty}\nentered = start\nfirst_tick = 0\nplayers = 1\n{route_extra}")
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


def test_catalog_accepts_experimental_simulation_settings(tmp_path):
    _recording(tmp_path)
    cases = _cases(players=2)
    cases["mp-chillout-proof"]["settings"] = {"debug.collision_type_filter": True, "debug.static_light_relink": False}
    cases["a10-opening"]["settings"] = {"debug.sphere_query_type_filter": True, "debug.immediate_merge": True,
                                        "debug.collision_candidates": True}
    _write_catalog(tmp_path, cases)
    loaded = {case["id"]: case for case in benchmark_catalog.load_catalog(tmp_path)}
    assert loaded["mp-chillout-proof"]["settings"] == {"debug.collision_type_filter": True,
                                                       "debug.static_light_relink": False}
    assert loaded["a10-opening"]["settings"] == {"debug.sphere_query_type_filter": True,
                                                 "debug.immediate_merge": True,
                                                 "debug.collision_candidates": True}


@pytest.mark.parametrize("settings", [
    {}, [], {"debug.enemy_multiplier": True}, {"debug.collision_type_filter": 1},
    {"debug.collision_type_filter": "true"}, {"display.shadow_map_size": True}])
def test_catalog_rejects_unsupported_settings(tmp_path, settings):
    _recording(tmp_path)
    cases = _cases(players=2)
    cases["mp-chillout-proof"]["settings"] = settings
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError, match="unsupported settings"):
        benchmark_catalog.load_catalog(tmp_path)


def test_catalog_accepts_four_player_handler_value(tmp_path):
    _recording(tmp_path)
    _write_catalog(tmp_path, _cases(players=4))
    assert next(case for case in benchmark_catalog.load_catalog(tmp_path) if case["kind"] == "multiplayer")["players"] == 4


@pytest.mark.parametrize("players", [2, 3, 16, 32, 64, 128])
def test_catalog_accepts_bounded_network_rosters(tmp_path, players):
    _write_catalog(tmp_path, _cases(players=players, gameplay=False))
    cases = benchmark_catalog.load_catalog(tmp_path)
    assert cases[-1]["players"] == players
    assert cases[-1]["simulation"] == "network_players"


def test_multiplayer_only_catalog_needs_no_campaign_recording(tmp_path):
    _write_catalog(tmp_path, _cases(gameplay=False))
    cases = benchmark_catalog.load_catalog(tmp_path)
    assert len(cases) == 3 and all(case["kind"] != "gameplay" for case in cases)


def test_catalog_rejects_network_roster_over_native_capacity(tmp_path):
    _write_catalog(tmp_path, _cases(players=129, gameplay=False))
    with pytest.raises(benchmark_catalog.CatalogError, match="players"):
        benchmark_catalog.load_catalog(tmp_path)


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
    (_cases(players=129), "players"),
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


def test_catalog_carries_enemy_multiplier_matching_its_route(tmp_path):
    _recording(tmp_path, route_extra="enemy_multiplier = 1.500\n")
    cases = _cases(players=2)
    cases["b30-warthog-fight"]["enemy_multiplier"] = 1.5
    _write_catalog(tmp_path, cases)
    loaded = benchmark_catalog.load_catalog(tmp_path)
    assert next(case for case in loaded if case["id"] == "b30-warthog-fight")["enemy_multiplier"] == 1.5


@pytest.mark.parametrize("route_extra,case_value", [
    ("enemy_multiplier = 2.000\n", None),
    ("", 2.0),
    ("enemy_multiplier = 2.000\n", 1.5),
])
def test_catalog_rejects_enemy_multiplier_that_differs_from_route(tmp_path, route_extra, case_value):
    _recording(tmp_path, route_extra=route_extra)
    cases = _cases(players=2)
    if case_value is not None:
        cases["b30-warthog-fight"]["enemy_multiplier"] = case_value
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError):
        benchmark_catalog.load_catalog(tmp_path)


@pytest.mark.parametrize("value", [0.5, 9, True, "2"])
def test_catalog_rejects_unsupported_enemy_multiplier(tmp_path, value):
    _recording(tmp_path)
    cases = _cases(players=2)
    cases["b30-warthog-fight"]["enemy_multiplier"] = value
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError):
        benchmark_catalog.load_catalog(tmp_path)


def test_catalog_rejects_enemy_multiplier_on_multiplayer_case(tmp_path):
    _recording(tmp_path)
    cases = _cases(players=2)
    cases["mp-chillout-proof"]["enemy_multiplier"] = 2
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError):
        benchmark_catalog.load_catalog(tmp_path)


def test_catalog_requires_whole_enemy_multiplier_as_integer(tmp_path):
    _recording(tmp_path, route_extra="enemy_multiplier = 2.000\n")
    cases = _cases(players=2)
    cases["b30-warthog-fight"]["enemy_multiplier"] = 2.0
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError):
        benchmark_catalog.load_catalog(tmp_path)
    cases["b30-warthog-fight"]["enemy_multiplier"] = 2
    _write_catalog(tmp_path, cases)
    benchmark_catalog.load_catalog(tmp_path)


EVENTS = "events = cinematic_start:1:30:a10_intro,cinematic_end:1:300:a10_intro,cinematic_end:2:15:b30_bridge\n"


def test_catalog_anchors_a_window_on_an_event_by_ordinal_or_script(tmp_path):
    for anchor in ("cinematic_end:2", "cinematic_end:b30_bridge"):
        _recording(tmp_path, route_extra=f"anchor = {anchor}\n{EVENTS}")
        cases = _cases(players=2)
        cases["b30-warthog-fight"]["anchor"] = anchor
        _write_catalog(tmp_path, cases)
        loaded = benchmark_catalog.load_catalog(tmp_path)
        assert next(c for c in loaded if c["id"] == "b30-warthog-fight")["anchor"] == anchor


def test_anchor_seconds_resolves_events():
    assert benchmark_catalog.anchor_seconds("cinematic_end:2", EVENTS[len("events = "):].strip()) == 0.5
    assert benchmark_catalog.anchor_seconds("cinematic_end:a10_intro", EVENTS[len("events = "):].strip()) == 10.0
    assert benchmark_catalog.anchor_seconds("checkpoint:1", EVENTS[len("events = "):].strip()) is None


@pytest.mark.parametrize("route_extra,anchor", [
    (EVENTS, "cinematic_end:2"),                         # the route does not carry the anchor
    ("anchor = cinematic_end:2\n" + EVENTS, None),       # the case does not
    ("anchor = cinematic_end:3\n" + EVENTS, "cinematic_end:3"),   # the event is not in the route
    ("anchor = cinematic_end:2\n" + EVENTS, "cinematic_end:1"),   # they differ
    ("anchor = checkpoint:1\n" + EVENTS, "checkpoint:1"),
])
def test_catalog_rejects_unresolvable_or_mismatched_anchor(tmp_path, route_extra, anchor):
    _recording(tmp_path, route_extra=route_extra)
    cases = _cases(players=2)
    if anchor:
        cases["b30-warthog-fight"]["anchor"] = anchor
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError):
        benchmark_catalog.load_catalog(tmp_path)


@pytest.mark.parametrize("anchor", ["Cinematic_end:2", "cinematic_end:0", "wave:1", "cinematic_end:", 3, None])
def test_catalog_rejects_malformed_anchor(tmp_path, anchor):
    _recording(tmp_path)
    cases = _cases(players=2)
    cases["b30-warthog-fight"]["anchor"] = anchor
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError):
        benchmark_catalog.load_catalog(tmp_path)


def test_catalog_accepts_display_settings(tmp_path):
    _recording(tmp_path)
    cases = _cases(players=2)
    cases["mp-chillout-proof"]["settings"] = {
        "display.render_height": 540, "display.anisotropic_filtering": 4, "display.shadow_map_size": 256,
        "display.model_lod": "auto", "display.anti_aliasing": "smaa", "display.effect_resolution": False,
        "display.mirror_resolution": "full", "display.render_scale": 0.67,
        "display.clutter_distance": 40, "debug.clutter_lowest_lod": True}
    _write_catalog(tmp_path, cases, raw=benchmark_catalog._canonical({"schema": 1, "cases": cases}).decode("utf-8"))
    loaded = {case["id"]: case for case in benchmark_catalog.load_catalog(tmp_path)}
    assert loaded["mp-chillout-proof"]["settings"]["display.render_height"] == 540


@pytest.mark.parametrize("settings", [
    {"display.render_height": -1}, {"display.render_height": True}, {"display.anisotropic_filtering": 3},
    {"display.anti_aliasing": "taa"}, {"display.render_scale": 1.5},
    {"display.compressed_textures": False}, {"display.clutter_distance": 201}, {"display.clutter_distance": -1},
    {"debug.clutter_lowest_lod": "on"}, {"display.model_lod": "high"}])
def test_catalog_rejects_unsupported_display_settings(tmp_path, settings):
    _recording(tmp_path)
    cases = _cases(players=2)
    cases["mp-chillout-proof"]["settings"] = settings
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError):
        benchmark_catalog.load_catalog(tmp_path)


def test_variant_replays_its_base_with_other_settings(tmp_path):
    _recording(tmp_path)
    cases = _cases(players=2)
    cases["b30-low"] = {"variant_of": "b30-warthog-fight", "order": 4, "settings": {"display.shadow_map_size": 128}}
    cases["mp-low"] = {"variant_of": "mp-chillout-proof", "order": 5, "settings": {"display.anti_aliasing": "fxaa"}}
    _write_catalog(tmp_path, cases)
    loaded = {case["id"]: case for case in benchmark_catalog.load_catalog(tmp_path)}
    assert loaded["b30-low"]["recording"] == "take-b30" and loaded["b30-low"]["variant_of"] == "b30-warthog-fight"
    assert loaded["b30-low"]["settings"] == {"display.shadow_map_size": 128}
    assert loaded["mp-low"]["players"] == 2 and "variant_of" in loaded["mp-low"]
    assert "variant_of" not in loaded["b30-warthog-fight"]


@pytest.mark.parametrize("variant", [
    {"variant_of": "nope", "order": 4, "settings": {"display.shadow_map_size": 128}},
    {"variant_of": "menu", "order": 4, "settings": {"display.shadow_map_size": 128}},
    {"variant_of": "b30-warthog-fight", "order": 4},
    {"variant_of": "b30-warthog-fight", "order": 4, "settings": {"display.shadow_map_size": 128}, "lead": 3},
    {"variant_of": "b30-again", "order": 4, "settings": {"display.shadow_map_size": 128}}])
def test_catalog_rejects_invalid_variants(tmp_path, variant):
    _recording(tmp_path)
    cases = _cases(players=2)
    cases["b30-again"] = {"variant_of": "b30-warthog-fight", "order": 6, "settings": {"display.shadow_map_size": 256}}
    cases["b30-low"] = variant
    _write_catalog(tmp_path, cases)
    with pytest.raises(benchmark_catalog.CatalogError):
        benchmark_catalog.load_catalog(tmp_path)
