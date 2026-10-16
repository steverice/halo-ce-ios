#!/usr/bin/env python3
"""Check the device profiles in profiles/ (see profiles/README.md): each file parses, sets only the settings a
profile may set (port/linux/src/port_profile_keys.h) with values they accept, and says why for each one; no two
model profiles name one device; and each platform's class profiles cover every GPU family once.

    python3 tools/profile_check.py [FILE.toml ...]     # the files named, or all of profiles/

Run by tools/ios_test.py. A drop-in file (device-profile.toml) can be checked the same way before it is sent."""
import json
from functools import lru_cache
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ROOT / "profiles"
KEYS = ROOT / "port/linux/src/port_profile_keys.h"
PLATFORMS = ("ios", "tvos", "visionos", "mac")
BASES = ("measured", "assumed", "inherited")
STATUSES = ("tested", "provisional", "below target")
# what a profile's top level may hold besides its settings' sections
FIELDS = {"schema", "name", "kind", "status", "match", "evidence", "scene", "external"}
# the scene classes a build reads; "gameplay" is the profile's own values, and a class keyed to a level or a
# cutscene ("cutscene@a10") is reserved
SCENES = ("menu", "cutscene")
MAXIMUM_BYTES = 64 * 1024
# the highest GPU family the class partition is checked up to (Apple9 is an A17 Pro, M3 or M4)
FAMILIES = range(1, 21)


class ParseError(Exception):
    pass


def toml_helper():
    """the helper that parses TOML as the app does (port/ios/tests/toml_json.c), built on first use"""
    helper = ROOT / "build/ios/probe/toml-json"
    sources = [ROOT / "port/ios/tests/toml_json.c", ROOT / "port/third_party/tomlc17/tomlc17.c"]
    if not helper.exists() or any(source.stat().st_mtime > helper.stat().st_mtime for source in sources):
        helper.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["cc", "-O1", f"-I{ROOT / 'port/third_party/tomlc17'}", *map(str, sources), "-o", str(helper)],
                       check=True)
    return helper


def load_toml(path):
    """path's table, by the app's own parser"""
    result = subprocess.run([str(toml_helper()), str(path)], capture_output=True, text=True)
    if result.returncode:
        raise ParseError(result.stderr.strip())
    return json.loads(result.stdout)


@lru_cache(maxsize=2)
def schema_validator(drop_in=False):
    """Optional editor-schema validation; generation never calls back into this path."""
    try:
        from jsonschema import Draft202012Validator
    except ImportError:
        return None
    path = ROOT / "port/settings/profile.schema.json"
    if not path.exists():
        return None  # Bootstrapping a source checkout before schema generation.
    schema = json.loads(path.read_text())
    if drop_in:
        # A local drop-in may omit model identities, unlike a bundled profile.
        schema["allOf"][0]["else"]["properties"]["match"].pop("anyOf", None)
    return Draft202012Validator(schema)


def scene_keys(text=None):
    """the settings a profile may also set for a scene class (PROFILE_SCENE in port_profile_keys.h)"""
    return set(re.findall(r'^PROFILE_SCENE\("([a-z_]+\.[a-z0-9_]+)"\)\s*$', KEYS.read_text() if text is None else text, re.M))


def external_keys(text=None):
    """Settings whose values can change with the output."""
    return set(re.findall(r'^PROFILE_EXTERNAL\("([a-z_]+\.[a-z0-9_]+)"\)\s*$', KEYS.read_text() if text is None else text, re.M))


def profile_keys(text=None):
    """{name: (type, minimum, maximum, choices)} from port_profile_keys.h"""
    text = KEYS.read_text() if text is None else text
    keys = {}
    for kind, name, rest in re.findall(r'^PROFILE_(BOOLEAN|INTEGER|REAL|STRING)\("([a-z_]+\.[a-z0-9_]+)"(.*)\)\s*$',
                                       text, re.M):
        arguments = [part.strip() for part in rest.split(",")[1:]]
        if kind == "BOOLEAN":
            keys[name] = ("boolean", None, None, None)
        elif kind == "STRING":
            keys[name] = ("string", None, None, arguments[0].strip('"').split("|"))
        else:
            keys[name] = (kind.lower(), float(arguments[0]), float(arguments[1]), None)
    if not keys:
        raise SystemExit(f"{KEYS}: no PROFILE_ entries found")
    return keys


def value_problem(name, value, key):
    """why value is not one name accepts, or None"""
    kind, minimum, maximum, choices = key
    if kind == "boolean":
        return None if isinstance(value, bool) else "must be true or false"
    if kind == "integer":
        if isinstance(value, bool) or not isinstance(value, int):
            return "must be a whole number"
    elif kind == "real":
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return "must be a number"
    if kind in ("integer", "real"):
        return None if minimum <= value <= maximum else f"must be from {minimum:g} to {maximum:g}"
    if not isinstance(value, str):
        return "must be a quoted string"
    if value in choices and value != "#":
        return None
    if "#" in choices and re.fullmatch(r"[1-9][0-9]{0,3}", value) and 30 <= int(value) <= 1000:
        return None
    return "must be one of " + ", ".join(f'"{choice}"' if choice != "#" else "a number from 30 to 1000 in quotes"
                                         for choice in choices)


PRODUCT_PLATFORMS = {"iPhone": "ios", "iPad": "ios", "AppleTV": "tvos", "RealityDevice": "visionos"}
MAC_TIERS = ("", " Pro", " Max", " Ultra")
MAXIMUM_GENERATION = 1000000


def newer_rule(match):
    """Return (product line, newest generation, tier) or reject an unsafe rule.

    A Mac tier is a floor across generations. Unknown tiers cannot establish it.
    Model minors never rank devices. Only explicit and_newer rules use this order;
    the hardware catalog retains its independent measured capability ordering.
    """
    mac = match.get("platform") == "mac"
    field, other = ("chips", "models") if mac else ("models", "chips")
    names = match.get(field)
    if not isinstance(names, list) or not 1 <= len(names) <= 32 or match.get(other):
        raise ValueError(f"and_newer needs only match.{field} identities")
    line, tier, generation = None, None, 0
    for name in names:
        if not isinstance(name, str) or len(name) > 48:
            raise ValueError("and_newer identities must be strings of at most 48 characters")
        parsed = re.fullmatch(r"Apple M([1-9][0-9]*)( Pro| Max| Ultra)?" if mac else
                              r"(iPhone|iPad|AppleTV|RealityDevice)([1-9][0-9]*),([0-9]+)", name)
        if not parsed:
            raise ValueError("and_newer needs recognized product lines or Apple M chip tiers")
        if mac:
            product, number, rank = "Apple M", int(parsed[1]), MAC_TIERS.index(parsed[2] or "")
        else:
            product, number, rank = parsed[1], int(parsed[2]), 0
            if PRODUCT_PLATFORMS[product] != match.get("platform") or int(parsed[3]) > MAXIMUM_GENERATION:
                raise ValueError("and_newer product line must match its platform and use bounded identifiers")
        if not 1 <= number <= MAXIMUM_GENERATION:
            raise ValueError("and_newer generation must be from 1 to 1000000")
        if line is not None and (product != line or rank != tier):
            raise ValueError("and_newer identities must share one product line and chip tier")
        line, tier, generation = product, rank, max(generation, number)
    return line, generation, tier


def check_profile(path, keys, drop_in=False, scenes=None):
    """the problems in one profile, and its parsed table (None if it does not parse)"""
    problems = []
    scenes = scene_keys() if scenes is None else scenes
    if path.stat().st_size > MAXIMUM_BYTES:
        problems.append(f"is over {MAXIMUM_BYTES} bytes; the app does not read it")
    try:
        table = load_toml(path)
    except ParseError as error:
        return [f"does not parse: {error}"], None
    if table.get("schema") != 1:
        problems.append("schema must be 1")
    if not isinstance(table.get("name"), str) or not table.get("name"):
        problems.append('needs name = "..."')
    kind = table.get("kind")
    if kind not in ("class", "model"):
        problems.append('kind must be "class" or "model"')
    if "status" in table and table["status"] not in STATUSES:
        problems.append("status must be one of " + ", ".join(STATUSES))
    match = table.get("match")
    if not isinstance(match, dict):
        problems.append("needs a [match] table")
        match = {}
    if match.get("platform") not in PLATFORMS:
        problems.append("match.platform must be one of " + ", ".join(PLATFORMS))
    if kind == "class":
        family = match.get("gpu_family")
        if (not isinstance(family, list) or len(family) != 2 or not all(type(n) is int for n in family)
                or family[0] < 1 or (family[1] and family[1] < family[0])):
            problems.append("a class needs match.gpu_family = [lowest, highest] (highest 0 for no upper bound)")
        if set(match) - {"platform", "gpu_family"}:
            problems.append("a class's [match] holds only platform and gpu_family")
    elif kind == "model":
        named = False
        for field in ("models", "chips"):
            names = match.get(field, [])
            if not isinstance(names, list) or not all(isinstance(n, str) and re.fullmatch(r"[A-Za-z0-9 ,.-]{1,48}", n)
                                                      for n in names):
                problems.append(f"match.{field} must be a list of names (letters, digits, spaces, commas, dots)")
            named = named or bool(names)
        if not named and not drop_in:
            problems.append("a model profile needs match.models or match.chips")
        if set(match) - {"platform", "models", "chips", "and_newer"}:
            problems.append("a model's [match] holds only platform, models, chips and and_newer")
    if "and_newer" in match:
        if kind != "model" or type(match["and_newer"]) is not bool:
            problems.append("match.and_newer must be a boolean on a model profile")
        elif match["and_newer"]:
            try:
                newer_rule(match)
            except ValueError as error:
                problems.append(str(error))
    evidence = table.get("evidence", {})
    if not isinstance(evidence, dict):
        problems.append("[evidence] must be tables named for settings")
        evidence = {}
    settings = []

    def check_settings(sections, scene, external=False):
        """a profile's own sections (scene None) or one scene class's: evidence is named "display.x" for the
        first and "scene.menu.display.x" for the second"""
        for section, values in sections.items():
            if scene is None and section in ({"scene"} if external else FIELDS):
                continue
            if not isinstance(values, dict):
                problems.append(f"unknown field {section}")
                continue
            for key, value in values.items():
                name = f"{section}.{key}"
                shown = name if scene is None else f"scene.{scene}.{name}"
                if external:
                    shown = "external." + shown
                if name not in keys:
                    problems.append(f"{shown} is not a setting a profile may set (see port/linux/src/port_profile_keys.h)")
                    continue
                if external and name not in external_keys():
                    problems.append(f"{shown}: {name} cannot change by output (PROFILE_EXTERNAL in port_profile_keys.h)")
                    continue
                if not external and name == "display.output_height":
                    problems.append(f"{shown} is valid only in an external section")
                    continue
                if scene is not None and name not in scenes:
                    problems.append(f"{shown}: {name} cannot change by scene class (PROFILE_SCENE in port_profile_keys.h)")
                    continue
                problem = value_problem(name, value, keys[name])
                if problem:
                    problems.append(f"{shown} = {value!r}: {problem}")
                settings.append(shown)

    def check_context(context, external=False):
        check_settings(context, None, external)
        scene_tables = context.get("scene", {})
        if not isinstance(scene_tables, dict):
            problems.append("[scene] must be tables named for scene classes: [scene.menu.display]")
            return
        for scene, sections in scene_tables.items():
            if scene not in SCENES or not isinstance(sections, dict):
                problems.append(f'scene class "{scene}": a build reads only ' + ", ".join(SCENES)
                                + " (the profile's own values are gameplay's)")
                continue
            check_settings(sections, scene, external)

    check_context(table)
    external = table.get("external", {})
    if not isinstance(external, dict):
        problems.append("[external] must be a table")
    else:
        check_context(external, True)
    for name in settings:
        entry = evidence.get(name)
        if not isinstance(entry, dict):
            problems.append(f'{name} needs an [evidence."{name}"] table saying why')
            continue
        if entry.get("basis") not in BASES:
            problems.append(f'evidence for {name}: basis must be one of ' + ", ".join(BASES))
        if not isinstance(entry.get("why"), str) or len(entry.get("why", "")) < 20:
            problems.append(f"evidence for {name}: why must say, in a sentence, why this value")
        if entry.get("basis") == "measured" and (not isinstance(entry.get("source"), str) or len(entry["source"]) < 10):
            problems.append(f"evidence for {name}: a measured value needs source (the report, the scene and the date)")
        if name.startswith("external.") and entry.get("basis") == "measured" and entry.get("output") != "external":
            problems.append(f'evidence for {name}: a measured external value needs output = "external"')
        allowed = {"basis", "why", "source", "output"} if name.startswith("external.") else {"basis", "why", "source"}
        if set(entry) - allowed:
            problems.append(f"evidence for {name}: only " + ", ".join(sorted(allowed)))
    for name in evidence:
        if name not in settings:
            problems.append(f"evidence for {name}, which this profile does not set")
    validator = schema_validator(drop_in)
    if validator is not None:
        for error in validator.iter_errors(table):
            location = ".".join(map(str, error.absolute_path)) or "profile"
            problems.append(f"{location}: {error.message}")
    return problems, table


def check_set(tables):
    """problems across a whole profiles/ folder: {file name: parsed table}"""
    problems = []
    claimed = {}
    rules = {}
    for file, table in sorted(tables.items()):
        match = table.get("match", {})
        if table.get("kind") != "model" or not isinstance(match, dict):
            continue
        if match.get("and_newer") is True:
            try:
                key = (match.get("platform"), *newer_rule(match))
                if key in rules:
                    problems.append(f"{file}: ambiguous and_newer threshold also declared by {rules[key]}")
                rules.setdefault(key, file)
            except ValueError:
                pass  # The per-file checker already reports malformed rules.
        for field in ("models", "chips"):
            for name in match.get(field, []) if isinstance(match.get(field, []), list) else []:
                key = (field, name)
                if key in claimed:
                    problems.append(f"{file}: {name} is already named by {claimed[key]}")
                claimed.setdefault(key, file)
    for platform in PLATFORMS:
        classes = [(file, table["match"]["gpu_family"]) for file, table in sorted(tables.items())
                   if table.get("kind") == "class" and isinstance(table.get("match"), dict)
                   and table["match"].get("platform") == platform
                   and isinstance(table["match"].get("gpu_family"), list) and len(table["match"]["gpu_family"]) == 2]
        if not classes:
            problems.append(f"no class profile for platform {platform}: a device without a model profile needs one")
            continue
        for family in FAMILIES:
            holders = [file for file, (low, high) in classes if low <= family and (high == 0 or family <= high)]
            if len(holders) != 1:
                problems.append(f"platform {platform}, GPU family Apple{family}: "
                                + ("no class profile covers it" if not holders else "covered by " + " and ".join(holders)))
                break
    return problems


def main(arguments):
    keys = profile_keys()
    files = [Path(argument) for argument in arguments] or sorted(PROFILES.glob("*.toml"))
    whole_folder = not arguments
    failed = False
    tables = {}
    valid_tables = {}
    for path in files:
        if not re.fullmatch(r"[a-z0-9]+(-[a-z0-9]+)*\.toml|TEMPLATE\.toml|device-profile\.toml", path.name):
            print(f"{path.name}: the file name must be lowercase words joined by hyphens, ending .toml")
            failed = True
        problems, table = check_profile(path, keys, drop_in=path.name == "device-profile.toml")
        for problem in problems:
            print(f"{path.name}: {problem}")
        failed = failed or bool(problems)
        if table is not None and path.name != "TEMPLATE.toml":
            tables[path.name] = table
            if not problems:
                valid_tables[path.name] = table
    if whole_folder:
        for problem in check_set(tables):
            print(problem)
            failed = True
        from profile_catalog_check import check_catalog
        for problem in check_catalog(PROFILES / "hardware/catalog.toml", valid_tables):
            print(f"hardware/catalog.toml: {problem}")
            failed = True
    if failed:
        return 1
    print(f"PASS: {len(files)} device profile{'s' if len(files) != 1 else ''} checked against {len(keys)} settings")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
