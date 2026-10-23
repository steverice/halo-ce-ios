"""Strict validation for benchmark cases carried by signed recording packs."""
import json
import math
import os
import re
import stat
import struct
from pathlib import Path

CATALOG_NAME = "benchmark-catalog.json"
CATALOG_SCHEMA = 1
MAXIMUM_CASES = 20
MAXIMUM_CATALOG_BYTES = 256 * 1024
CASE_ID = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,45}[a-z0-9])?\Z")
MAP_NAME = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,29}[a-z0-9])?\Z")
RECORDING_NAME = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,46}[a-z0-9])?\Z")
DIFFICULTIES = {"easy", "normal", "hard", "impossible"}
KINDS = {"menu", "cutscene", "gameplay", "multiplayer"}
PLAYERS = range(2, 129)
COMMON_FIELDS = {"kind", "order", "map", "difficulty", "recording", "lead", "window"}
MAXIMUM_ENEMY_MULTIPLIER = 8.0
# a window anchored on an event of the recording: "kind:n" (the nth) or "kind:script" (the first one that script made)
ANCHOR = re.compile(r"(cinematic_start|cinematic_end|cinematic_done|checkpoint|bsp_switch):([1-9][0-9]{0,2}|[a-z][a-z0-9_]{0,30})\Z")


def anchor_seconds(anchor, events_text):
    """The recorded time, in seconds, of the event a route's anchor names (its 'events = kind:n:tick[:script],...'
    line), or None when the route does not have it."""
    kind, _, which = anchor.partition(":")
    for entry in events_text.split(","):
        parts = entry.split(":")
        if len(parts) < 3 or parts[0] != kind or not parts[2].isdigit():
            continue
        if (parts[1] == which if which.isdigit() else (len(parts) > 3 and parts[3] == which)):
            return int(parts[2]) / 30.0
    return None
# a case's optional "settings": values a run sets in place of the profile's, by config name. The experimental exact
# simulation changes are booleans (bit N of the scene's sim_experiments is the Nth key; port/linux/game/sim_experiments.c)
# and change work done, not results. A setting a later change adds takes one entry here, one in
# host_benchmark_catalog.m and its application in the guest (port/linux/src/benchmark_suite.c).
SIM_SETTINGS = ("debug.collision_type_filter", "debug.sphere_query_type_filter", "debug.static_light_relink",
                "debug.collision_candidates")
# name: ("boolean",) | ("integer", allowed values or None, minimum, maximum) | ("number", minimum, maximum) | ("text", allowed)
CASE_SETTINGS = {
    **{name: ("boolean",) for name in SIM_SETTINGS},
    # (0: the display's own height)
    "display.render_height": ("integer", None, 0, 4320),
    "display.anisotropic_filtering": ("integer", (1, 2, 4, 8, 16), 1, 16),
    "display.shadow_map_size": ("integer", (128, 256, 512, 1024, 2048), 128, 2048),
    "display.model_lod": ("text", ("max", "auto")),
    "display.anti_aliasing": ("text", ("off", "fxaa", "smaa")),
    "display.effect_resolution": ("text", ("xbox", "half", "full")),
    "display.render_scale": ("number", 0.25, 1.0),
    # (0: no limit)
    "display.clutter_distance": ("number", 0.0, 200.0),
    "debug.clutter_lowest_lod": ("boolean",),
    "debug.offscreen_window_wall_seconds": ("number", 30.0, 1800.0),
}


def _setting_valid(name, value):
    spec = CASE_SETTINGS.get(name)
    if spec is None:
        return False
    kind = spec[0]
    if kind == "boolean":
        return isinstance(value, bool)
    if kind == "text":
        return isinstance(value, str) and value in spec[1]
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        return False
    if kind == "integer":
        return isinstance(value, int) and spec[2] <= value <= spec[3] and (spec[1] is None or value in spec[1])
    return spec[1] <= value <= spec[2]


MULTIPLAYER_FIELDS = {"simulation", "players", "views", "game_mode", "score_limit", "time_limit_minutes"}


class CatalogError(ValueError):
    """The signed benchmark catalog is missing or malformed."""


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise CatalogError(f"catalog contains duplicate JSON key {key!r}")
        result[key] = value
    return result


def _read_regular(path, maximum):
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError as error:
        raise CatalogError(f"{path.name} is missing") from error
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= maximum:
            raise CatalogError(f"{path.name} is not a bounded regular file")
        chunks = []
        remaining = info.st_size
        while remaining:
            chunk = os.read(descriptor, min(65536, remaining))
            if not chunk:
                raise CatalogError(f"{path.name} changed while reading")
            chunks.append(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise CatalogError(f"{path.name} changed while reading")
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def _catalog_key(key):
    # Foundation's sorted keys use numeric, case-insensitive, forced ordering
    # with the system locale. Catalog keys are restricted to lowercase ASCII;
    # punctuation sorts as underscore, hyphen, period before digits and letters.
    # Compare numeric runs by value, then break otherwise equal ties by width.
    if not re.fullmatch(r"[a-z0-9_.-]+", key):
        raise CatalogError("catalog case ID and field keys must use lowercase ASCII identifiers")
    parts = re.findall(r"[0-9]+|[^0-9]", key)
    primary = tuple((3, int(part)) if part.isdigit() else
                    ({"_": 0, "-": 1, ".": 2}.get(part, ord(part)), 0) for part in parts)
    return primary, tuple(len(part) for part in parts if part.isdigit())


def _canonical(value):
    """Foundation NSJSONWritingSortedKeys bytes for the catalog's ASCII grammar."""
    def encode(item):
        if isinstance(item, dict):
            return "{" + ",".join(encode(key) + ":" + encode(item[key])
                                  for key in sorted(item, key=_catalog_key)) + "}"
        if isinstance(item, list):
            return "[" + ",".join(encode(child) for child in item) + "]"
        if isinstance(item, float):
            if not math.isfinite(item):
                raise CatalogError("catalog numbers must be finite")
            return str(int(item)) if item.is_integer() else format(item, ".17g")
        # NSJSONSerialization escapes slashes by default, unlike json.dumps.
        return json.dumps(item, ensure_ascii=False, allow_nan=False).replace("/", "\\/")
    return encode(value).encode("utf-8")


def _number(value, name, minimum, maximum, *, positive=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise CatalogError(f"catalog {name} must be a finite number")
    if value < minimum or value > maximum or (positive and value == 0):
        raise CatalogError(f"catalog {name} is outside its supported range")
    return float(value)


def _text(value, name, pattern, *, empty=False):
    if not isinstance(value, str) or (not value and not empty) or (value and not pattern.fullmatch(value)):
        raise CatalogError(f"catalog {name} is invalid")
    return value


def _route(folder, recording):
    route_path = folder / f"{recording}.route"
    route_bytes = _read_regular(route_path, 4095)
    try:
        route_text = route_bytes.decode("ascii")
    except UnicodeDecodeError as error:
        raise CatalogError(f"{recording}.route is not ASCII") from error
    values = {}
    segment_seeds = []
    for line in route_text.splitlines():
        if not line:
            continue
        if " = " not in line:
            raise CatalogError(f"{recording}.route has a malformed field")
        key, value = line.split(" = ", 1)
        if key == "segment_seed":
            segment_seeds.append(value)
            continue
        if key in values:
            raise CatalogError(f"{recording}.route repeats {key}")
        values[key] = value
    try:
        route = {
            "scene": values.get("scene", recording),
            "kind": values.get("kind", "gameplay"),
            "map": values["map"],
            "difficulty": values["difficulty"],
            "entered": values["entered"],
            "first_tick": int(values["first_tick"]),
            "lead": float(values.get("lead", 0)),
            "window": float(values.get("window", values.get("seconds", 0))),
            "enemy_multiplier": float(values.get("enemy_multiplier", 1)),
            "anchor": values.get("anchor"),
            "events": values.get("events", ""),
        }
    except (KeyError, ValueError) as error:
        raise CatalogError(f"{recording}.route lacks required replay fields") from error
    if (route["kind"] not in {"gameplay", "cutscene"} or route["entered"] not in {"start", "console"} or
            route["first_tick"] not in {0, 1} or not math.isfinite(route["lead"]) or not math.isfinite(route["window"]) or
            not 1.0 <= route["enemy_multiplier"] <= MAXIMUM_ENEMY_MULTIPLIER):
        raise CatalogError(f"{recording}.route is not a fresh replayable route")
    for extension in ("input", "actions", "seeds"):
        _read_regular(folder / f"{recording}.{extension}", 4 * 1024 * 1024)
    return route


def _active_window(folder, recording, lead, window):
    path = folder / f"{recording}.actions"
    data = _read_regular(path, 4 * 1024 * 1024)
    try:
        lines = data.decode("ascii").splitlines()
    except UnicodeDecodeError as error:
        raise CatalogError(f"{recording}.actions is not ASCII") from error
    if not lines or not lines[0].startswith("halo action recording 1"):
        raise CatalogError(f"{recording}.actions has an invalid header")
    start, end = lead * 30.0, (lead + window) * 30.0
    active = False
    for ordinal, line in enumerate(lines[1:]):
        if ordinal < start or ordinal > end:
            continue
        fields = line.split()
        if len(fields) != 10:
            raise CatalogError(f"{recording}.actions has a malformed action")
        try:
            words = [int(word, 16) for word in fields[2:]]
            throttle = (struct.unpack("<f", struct.pack("<I", words[3]))[0],
                        struct.unpack("<f", struct.pack("<I", words[4]))[0])
            trigger = struct.unpack("<f", struct.pack("<I", words[5]))[0]
        except (ValueError, struct.error) as error:
            raise CatalogError(f"{recording}.actions has an invalid player action") from error
        if all(math.isfinite(value) for value in (*throttle, trigger)) and (math.hypot(*throttle) > 0.1 or trigger > 0.05):
            active = True
    return active


def _resolve_variants(cases):
    """Each case as (id, fields). A variant, {"variant_of": id, "order": n, "settings": {...}}, is the named case run again
    with its own settings: it takes that case's fields (its recording and route included) and says which in "variant_of"."""
    resolved = []
    for case_id, case in cases.items():
        if not isinstance(case, dict) or "variant_of" not in case:
            resolved.append((case_id, dict(case) if isinstance(case, dict) else case))
            continue
        base = cases.get(case.get("variant_of")) if isinstance(case.get("variant_of"), str) else None
        if (set(case) != {"variant_of", "order", "settings"} or not isinstance(base, dict) or "variant_of" in base or
                base.get("kind") == "menu"):
            raise CatalogError(f"case {case_id} is not a valid variant")
        resolved.append((case_id, {**base, "order": case["order"], "settings": case["settings"],
                                   "variant_of": case["variant_of"]}))
    return resolved


def load_catalog(folder):
    """Load cases in catalog order and validate their exact route references and selected windows."""
    root = Path(folder)
    try:
        raw = _read_regular(root / CATALOG_NAME, MAXIMUM_CATALOG_BYTES)
        document = json.loads(raw.decode("utf-8"), object_pairs_hook=_pairs,
                              parse_constant=lambda value: (_ for _ in ()).throw(CatalogError(f"invalid number {value}")))
    except FileNotFoundError as error:
        raise CatalogError("signed benchmark catalog is missing") from error
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CatalogError("benchmark catalog is not valid UTF-8 JSON") from error
    if _canonical(document) != raw:
        raise CatalogError("benchmark catalog must use canonical JSON")
    if not isinstance(document, dict) or set(document) != {"schema", "cases"} or document.get("schema") != CATALOG_SCHEMA:
        raise CatalogError("benchmark catalog has an unsupported schema or fields")
    cases = document["cases"]
    if not isinstance(cases, dict) or not 1 <= len(cases) <= MAXIMUM_CASES:
        raise CatalogError(f"benchmark catalog must contain 1 to {MAXIMUM_CASES} cases")
    prepared = []
    orders = set()
    recorded = set()
    has_gameplay = False
    for case_id, case in _resolve_variants(cases):
        _text(case_id, "case ID", CASE_ID)
        if not isinstance(case, dict) or "kind" not in case:
            raise CatalogError(f"case {case_id} must be an object with a kind")
        base_id = case.pop("variant_of", case_id)
        kind = case["kind"]
        if not isinstance(kind, str) or kind not in KINDS:
            raise CatalogError(f"case {case_id} has an unsupported kind")
        fields = COMMON_FIELDS | (MULTIPLAYER_FIELDS if kind == "multiplayer" else set())
        if kind == "multiplayer" and "arena" in case:
            fields |= {"arena"}
        if "enemy_multiplier" in case:
            fields |= {"enemy_multiplier"}
        if "anchor" in case:
            fields |= {"anchor"}
        if "settings" in case:
            fields |= {"settings"}
        if set(case) != fields:
            raise CatalogError(f"case {case_id} has missing or unsupported fields")
        order = case["order"]
        if isinstance(order, bool) or not isinstance(order, int) or not 0 <= order < MAXIMUM_CASES or order in orders:
            raise CatalogError(f"case {case_id} has an invalid or duplicate order")
        orders.add(order)
        map_name = _text(case["map"], f"{case_id} map", MAP_NAME, empty=kind == "menu")
        difficulty = case["difficulty"]
        if not isinstance(difficulty, str) or difficulty not in DIFFICULTIES:
            raise CatalogError(f"case {case_id} has an unsupported difficulty")
        if "enemy_multiplier" in case:
            if kind in {"menu", "multiplayer"}:
                raise CatalogError(f"case {case_id} cannot carry an enemy multiplier")
            value = _number(case["enemy_multiplier"], f"{case_id} enemy multiplier", 1, MAXIMUM_ENEMY_MULTIPLIER)
            if isinstance(case["enemy_multiplier"], float) and value.is_integer():
                # (the app's canonical JSON writes a whole number without a fraction)
                raise CatalogError(f"catalog {case_id} enemy multiplier must be written as an integer")
        anchor = case.get("anchor")
        if "anchor" in case and (kind in {"menu", "multiplayer"} or case["recording"] is None or
                                 not isinstance(anchor, str) or not ANCHOR.match(anchor)):
            raise CatalogError(f"case {case_id} has an unsupported anchor")
        if "settings" in case:
            settings = case["settings"]
            if (not isinstance(settings, dict) or not settings or
                    not all(_setting_valid(name, value) for name, value in settings.items())):
                raise CatalogError(f"case {case_id} has unsupported settings")
        lead = _number(case["lead"], f"{case_id} lead", 0, 3600)
        window = _number(case["window"], f"{case_id} window", 0, 3600, positive=True)
        recording = case["recording"]
        anchored = None
        if recording is not None:
            _text(recording, f"{case_id} recording", RECORDING_NAME)
            if base_id == case_id:
                if recording in recorded:
                    raise CatalogError(f"catalog selects the same recording for more than one case: {recording}")
                recorded.add(recording)
            route = _route(root, recording)
            if (route["scene"] != base_id or route["map"] != map_name or route["difficulty"] != difficulty or
                    route["kind"] != kind or abs(route["lead"] - lead) > 1e-6 or abs(route["window"] - window) > 1e-6 or
                    abs(route["enemy_multiplier"] - float(case.get("enemy_multiplier", 1))) > 1e-6 or
                    route["anchor"] != anchor):
                raise CatalogError(f"case {case_id} does not match its recording route")
            if anchor is not None:
                anchored = anchor_seconds(anchor, route["events"])
                if anchored is None:
                    raise CatalogError(f"case {case_id} anchors on an event its recording route does not have")
        elif kind == "gameplay":
            raise CatalogError(f"gameplay case {case_id} requires a recording")
        elif kind in {"cutscene", "multiplayer"} and not map_name:
            raise CatalogError(f"case {case_id} requires a map")
        elif kind == "menu" and (map_name or recording is not None):
            raise CatalogError(f"menu case {case_id} cannot select a map or recording")
        if kind == "multiplayer":
            simulation = case["simulation"]
            players = case["players"]
            if not isinstance(simulation, str) or simulation not in {"local_controllers", "network_players"}:
                raise CatalogError(f"multiplayer case {case_id} has an unsupported simulation mode")
            if isinstance(players, bool) or not isinstance(players, int) or players not in PLAYERS:
                raise CatalogError(f"multiplayer case {case_id} has an unsupported players value")
            if (simulation == "local_controllers" and players != 4) or (simulation == "network_players" and not 2 <= players <= 128):
                raise CatalogError(f"multiplayer case {case_id} players do not match its simulation mode")
            arena = case.get("arena")
            if players > 4 and arena is None:
                raise CatalogError(f"multiplayer case {case_id} requires a signed arena")
            if arena is not None:
                ranges = {"columns": (1, 8), "spacing_milliunits": (800, 3000),
                          "distance_milliunits": (2000, 12000), "movement_radius_milliunits": (150, 1000)}
                if simulation != "network_players" or not isinstance(arena, dict) or set(arena) != set(ranges):
                    raise CatalogError(f"multiplayer case {case_id} has an unsupported arena")
                for field, (minimum, maximum) in ranges.items():
                    value = arena[field]
                    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
                        raise CatalogError(f"multiplayer case {case_id} has invalid arena {field}")
                if arena["movement_radius_milliunits"] * 3 > arena["spacing_milliunits"]:
                    raise CatalogError(f"multiplayer case {case_id} has overlapping arena movement bounds")
            if isinstance(case["views"], bool) or not isinstance(case["views"], int) or case["views"] != 1:
                raise CatalogError(f"multiplayer case {case_id} requests an unsupported view count")
            if not isinstance(case["game_mode"], str) or case["game_mode"] != "slayer":
                raise CatalogError(f"multiplayer case {case_id} requests an unsupported game mode")
            score = case["score_limit"]
            if isinstance(score, bool) or not isinstance(score, int) or not 1 <= score <= 1000:
                raise CatalogError(f"multiplayer case {case_id} has an unsupported score limit")
            time_limit = case["time_limit_minutes"]
            if isinstance(time_limit, bool) or not isinstance(time_limit, int) or not 0 <= time_limit <= 120:
                raise CatalogError(f"multiplayer case {case_id} has an unsupported time limit")
        if kind == "gameplay":
            has_gameplay = True
            if not _active_window(root, recording, lead + (anchored or 0.0), window):
                raise CatalogError(f"gameplay case {case_id} has no movement or combat input in its measured window")
        prepared.append({"id": case_id, **case, **({"variant_of": base_id} if base_id != case_id else {})})
    if orders != set(range(len(cases))):
        raise CatalogError("catalog order must be contiguous from zero")
    if not has_gameplay and not any(case["kind"] == "multiplayer" for case in prepared):
        raise CatalogError("catalog must include gameplay or a supported multiplayer workload")
    prepared.sort(key=lambda case: case["order"])
    return prepared
