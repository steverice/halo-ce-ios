#!/usr/bin/env python3
"""Validate profiles/hardware/catalog.toml against the checked device profiles."""
import re
import sys
from pathlib import Path

import profile_check

MAXIMUM_BYTES = 64 * 1024
IDENTITY = re.compile(r"[A-Za-z0-9 ,.-]{1,48}")
HARDWARE_ID = re.compile(r"[A-Za-z0-9_-]{1,48}")
FILENAME = re.compile(r"[a-z0-9]+(-[a-z0-9]+)*\.toml")
HARDWARE_FIELDS = {"id", "platform", "models", "chips", "tier", "gpu_family", "basis", "source"}
FALLBACK_FIELDS = {"hardware", "profile", "minimum_memory_mb", "minimum_gpu_family",
                   "maximum_pixels", "maximum_refresh_hertz"}


def integer(value, minimum, maximum=None):
    return type(value) is int and value >= minimum and (maximum is None or value <= maximum)


def text(value, maximum=None):
    return isinstance(value, str) and bool(value.strip()) and (maximum is None or len(value) <= maximum)


def check_table(table, profiles):
    """Return every catalog problem; profiles contains only successfully checked profiles."""
    problems = []

    def unknown(entry, allowed, label):
        for field in sorted(set(entry) - allowed):
            problems.append(f"{label}: unknown field {field}")

    def number(entry, field, minimum, maximum, label):
        if not integer(entry.get(field), minimum, maximum):
            bound = f" to {maximum}" if maximum is not None else " or greater"
            problems.append(f"{label}.{field} must be an integer from {minimum}{bound}")

    if not isinstance(table, dict):
        return ["catalog must be a table"]
    unknown(table, {"schema", "version", "hardware", "fallback"}, "catalog")
    if type(table.get("schema")) is not int or table.get("schema") != 1:
        problems.append("schema must be 1")
    if not text(table.get("version")) or len(table["version"].encode("utf-8")) > 48:
        problems.append("version must be a nonempty string of at most 48 UTF-8 bytes")
    arrays = {}
    for field in ("hardware", "fallback"):
        values = table.get(field)
        if not isinstance(values, list) or len(values) > 128:
            problems.append(f"{field} must be an array of at most 128 tables")
            arrays[field] = []
        else:
            arrays[field] = values
    hardware = {}
    identities = []
    for index, entry in enumerate(arrays["hardware"]):
        label = f"hardware[{index}]"
        if not isinstance(entry, dict):
            problems.append(f"{label} must be a table")
            continue
        unknown(entry, HARDWARE_FIELDS, label)
        identifier = entry.get("id")
        if not isinstance(identifier, str) or not HARDWARE_ID.fullmatch(identifier):
            problems.append(f"{label}.id must be 1 to 48 ASCII letters, digits, underscores or hyphens")
        elif identifier in hardware:
            problems.append(f"{label}: duplicate id {identifier}")
        else:
            hardware[identifier] = entry
        platform = entry.get("platform")
        if platform not in profile_check.PLATFORMS:
            problems.append(f"{label}.platform must be one of " + ", ".join(profile_check.PLATFORMS))
        number(entry, "gpu_family", 1, 32, label)
        if "tier" in entry:
            number(entry, "tier", 1, 1000000, label)
        if entry.get("basis") not in profile_check.BASES:
            problems.append(f"{label}.basis must be one of " + ", ".join(profile_check.BASES))
        if not text(entry.get("source")):
            problems.append(f"{label}.source must be a nonempty evidence string")
        names = {}
        for field in ("models", "chips"):
            values = entry.get(field, [])
            if (not isinstance(values, list) or len(values) > 32
                    or not all(isinstance(value, str) and IDENTITY.fullmatch(value) for value in values)):
                problems.append(f"{label}.{field} must be at most 32 identity strings of 1 to 48 characters")
                names[field] = []
            else:
                names[field] = values
                if len(set(values)) != len(values):
                    problems.append(f"{label}.{field} has duplicate identities")
        if not names["models"] and not names["chips"]:
            problems.append(f"{label} needs at least one model or chip identity")
        # Models and chips both constrain the match; an absent list accepts any identity.
        if platform in profile_check.PLATFORMS:
            for previous_platform, previous_label, previous_names in identities:
                if platform == previous_platform and all(
                        not names[field] or not previous_names[field]
                        or not set(names[field]).isdisjoint(previous_names[field])
                        for field in ("models", "chips")):
                    problems.append(f"{label}: ambiguous hardware identity overlapping {previous_label}")
            identities.append((platform, label, names))

    sources = set()
    for index, entry in enumerate(arrays["fallback"]):
        label = f"fallback[{index}]"
        if not isinstance(entry, dict):
            problems.append(f"{label} must be a table")
            continue
        unknown(entry, FALLBACK_FIELDS, label)
        number(entry, "minimum_memory_mb", 0, 1000000, label)
        number(entry, "minimum_gpu_family", 1, 32, label)
        number(entry, "maximum_refresh_hertz", 1, 1000, label)
        pixels = entry.get("maximum_pixels")
        if (not isinstance(pixels, list) or len(pixels) != 3
                or not all(integer(value, 1, 100000000) for value in pixels)):
            problems.append(f"{label}.maximum_pixels must be three positive integers at most 100000000")
        identifier = entry.get("hardware")
        source = hardware.get(identifier) if isinstance(identifier, str) else None
        if source is None:
            problems.append(f"{label}: unknown hardware {identifier!r}")
        else:
            if not integer(source.get("tier"), 1, 1000000):
                problems.append(f"{label}: source hardware must have a valid tier")
            elif source.get("platform") in profile_check.PLATFORMS:
                key = (source["platform"], source["tier"])
                if key in sources:
                    problems.append(f"{label}: duplicate fallback for platform {key[0]}, tier {key[1]}")
                sources.add(key)
        filename = entry.get("profile")
        profile = (profiles.get(filename) if isinstance(filename, str) and FILENAME.fullmatch(filename)
                   and len(filename.encode("utf-8")) < 64 else None)
        if profile is None:
            problems.append(f"{label}.profile must reference a valid model profile filename")
            continue
        if profile.get("kind") != "model":
            problems.append(f"{label}.profile must have kind = model")
            continue
        if source is None:
            continue
        match = profile.get("match", {})
        if match.get("platform") != source.get("platform"):
            problems.append(f"{label}: profile and source hardware must have the same platform")
        intersection = any(isinstance(source.get(field, []), list) and isinstance(match.get(field, []), list)
                           and any(isinstance(value, str) and value in match.get(field, [])
                                   for value in source.get(field, [])) for field in ("models", "chips"))
        if not intersection:
            problems.append(f"{label}: source identity must intersect the profile match")
        family = source.get("gpu_family")
        classes = []
        for name, candidate in profiles.items():
            candidate_match = candidate.get("match", {})
            bounds = candidate_match.get("gpu_family")
            if (candidate.get("kind") == "class" and candidate_match.get("platform") == source.get("platform")
                    and isinstance(bounds, list) and len(bounds) == 2
                    and all(type(value) is int for value in bounds) and integer(family, 1, 32)
                    and bounds[0] <= family and (bounds[1] == 0 or family <= bounds[1])):
                classes.append(name)
        if len(classes) != 1:
            problems.append(f"{label}: needs exactly one source class covering GPU family {family!r}")
    return problems


def check_catalog(path, profiles=None):
    """Check one bounded catalog and its references; never accept unreadable or malformed data."""
    path = Path(path)
    try:
        if path.stat().st_size > MAXIMUM_BYTES:
            return [f"catalog is over {MAXIMUM_BYTES} bytes; the app does not read it"]
        table = profile_check.load_toml(path)
    except (OSError, profile_check.ParseError) as error:
        return [f"cannot read or parse catalog: {error}"]
    if profiles is None:
        profiles = {}
        keys = profile_check.profile_keys()
        for file in sorted(path.parent.parent.glob("*.toml")):
            if file.name == "TEMPLATE.toml":
                continue
            errors, profile = profile_check.check_profile(file, keys)
            if not errors and profile is not None:
                profiles[file.name] = profile
    return check_table(table, profiles)


def main(arguments):
    if len(arguments) > 1:
        print("usage: profile_catalog_check.py [CATALOG.toml]")
        return 2
    path = Path(arguments[0]) if arguments else profile_check.PROFILES / "hardware/catalog.toml"
    problems = check_catalog(path)
    for problem in problems:
        print(f"{path.name}: {problem}")
    if not problems:
        print("PASS: hardware catalog checked against device profiles")
    return int(bool(problems))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
