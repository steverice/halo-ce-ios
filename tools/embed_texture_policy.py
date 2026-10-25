#!/usr/bin/env python3
"""port/assets/texture-policy.json as a C table (port/runtime/texture_policy.h), the way tools/embed_assets.py
embeds the HUD layout: the policy changes only with an app build, and the player cannot edit it.

  embed_texture_policy.py POLICY_JSON OUTPUT_C"""

import json
import sys
from pathlib import Path

RESULTS = {"s4g": "TEXTURE_POLICY_S4G", "bpf": "TEXTURE_POLICY_BPF", "original": "TEXTURE_POLICY_ORIGINAL"}


def string(text: str) -> str:
    if not text.isascii():
        raise SystemExit(f"embed_texture_policy: {text!r} is not ASCII")
    return json.dumps(text)          # a valid C string literal for ASCII: \\ and \" escaped


def array(name: str, values: list[str], lines: list[str]) -> str:
    if not values:
        return "NULL, 0"
    lines.append(f"static const char *const {name}[] = {{ {', '.join(string(v) for v in values)} }};")
    return f"{name}, {len(values)}"


DAMAGE_FIELDS = {"share", "structure_loss", "bpf_structure_loss", "measure_version"}


def damage_values(source: Path, damage) -> str:
    """the "damage" block as the table's last four fields; a null threshold (a test turned off) is -1"""
    if not isinstance(damage, dict) or set(damage) != DAMAGE_FIELDS:
        raise SystemExit(f"embed_texture_policy: {source}: damage needs exactly {sorted(DAMAGE_FIELDS)}")
    values = []
    for name in ("share", "structure_loss", "bpf_structure_loss"):
        value = damage[name]
        if value is None:
            values.append("-1.0f")
        elif isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 1:
            raise SystemExit(f"embed_texture_policy: {source}: damage {name} must be a number in [0, 1] or null")
        else:
            values.append(f"{float(value)}f")
    version = damage["measure_version"]
    if not isinstance(version, int) or isinstance(version, bool) or version < 1:
        raise SystemExit(f"embed_texture_policy: {source}: damage measure_version must be an integer of at least 1")
    return f"{', '.join(values)}, {version}"


def material_values(source: Path, raw: dict, lines: list[str]) -> str:
    """"material_names" and "material_result" as the table's last three fields"""
    names, result = raw.get("material_names"), raw.get("material_result")
    if not isinstance(names, list) or not all(isinstance(n, str) and n for n in names):
        raise SystemExit(f"embed_texture_policy: {source}: material_names must be a list of non-empty strings")
    if result not in RESULTS:
        raise SystemExit(f"embed_texture_policy: {source}: material_result must be one of {sorted(RESULTS)}")
    return f"{array('material_names', names, lines)}, {RESULTS[result]}"


KINDS = ["graphic", "surface", "companion", "bump", "original"]          # enum texture_policy_kind
TREATMENTS = ["skip", "color", "first-person", "emissive", "multipurpose", "bump"]   # enum texture_policy_treatment
SCALES = {1, 2, 4, 8}
SCALE_FIELDS = {"base", "first_person", "doubled"}
DOUBLED_FIELDS = {"kinds", "treatments", "tags", "textures", "reason"}


def scale_values(source: Path, scales, lines: list[str]) -> str:
    """"scales" as the table's last eight fields"""
    def fail(problem):
        raise SystemExit(f"embed_texture_policy: {source}: scales {problem}")
    if not isinstance(scales, dict) or set(scales) != SCALE_FIELDS:
        fail(f"needs exactly {sorted(SCALE_FIELDS)}")
    base, first_person, doubled = scales["base"], scales["first_person"], scales["doubled"]
    for name, value in (("base", base), ("first_person", first_person)):
        if value not in SCALES or isinstance(value, bool) or value * 2 > 8:
            fail(f"{name} must be 1, 2 or 4 (doubled, at most 8)")
    if not isinstance(doubled, dict) or set(doubled) != DOUBLED_FIELDS:
        fail(f"doubled needs exactly {sorted(DOUBLED_FIELDS)}")
    if not all(k in KINDS for k in doubled["kinds"]) or not all(t in TREATMENTS for t in doubled["treatments"]):
        fail(f"doubled kinds must be in {KINDS} and treatments in {TREATMENTS}")
    kinds = sum(1 << KINDS.index(k) for k in doubled["kinds"])
    treatments = sum(1 << TREATMENTS.index(t) for t in doubled["treatments"])
    tags = array("doubled_tags", doubled["tags"], lines)
    keys = []
    for key in doubled["textures"]:
        try:
            digest, size = key.split("-")
            width, height = (int(v) for v in size.split("x"))
            int(digest, 16)
            if len(digest) != 16 or digest != digest.lower():
                raise ValueError
        except ValueError:
            fail(f"doubled texture {key!r} is not HASH-WIDTHxHEIGHT (16 lowercase hex digits)")
        keys.append(f"{{ 0x{digest}ull, {width}, {height} }}")
    if keys:
        lines.append("static const struct texture_policy_key doubled_textures[] = {")
        lines += [f"\t{key}," for key in keys]
        lines.append("};")
        textures = f"doubled_textures, {len(keys)}"
    else:
        textures = "NULL, 0"
    return f"{base}, {first_person}, {kinds:#x}, {treatments:#x}, {tags}, {textures}"


def main() -> None:
    source, output = Path(sys.argv[1]), Path(sys.argv[2])
    raw = json.loads(source.read_text())
    if raw.get("schema") != 1:
        raise SystemExit(f"embed_texture_policy: {source}: schema {raw.get('schema')} is not 1")
    damage = damage_values(source, raw.get("damage"))
    lines = [f"/* generated by tools/embed_texture_policy.py from {source.name}; do not edit */",
             '#include "texture_policy.h"', ""]
    names = array("graphic_names", raw["graphic_names"], lines)
    groups = array("graphic_groups", raw["graphic_groups"], lines)
    prefixes = array("decal_surface_prefixes", raw["decal_surface_prefixes"], lines)
    materials = material_values(source, raw, lines)
    scales = scale_values(source, raw.get("scales"), lines)
    if raw["overrides"]:
        lines.append("static const struct texture_policy_override overrides[] = {")
        for o in raw["overrides"]:
            digest = string(o["hash"]) if "hash" in o else "NULL"
            lines.append(f"\t{{ {string(o['tag'])}, {o.get('index', -1)}, {digest}, {RESULTS[o['result']]} }},")
        lines.append("};")
        overrides = f"overrides, {len(raw['overrides'])}"
    else:
        overrides = "NULL, 0"
    r, f = raw["results"], raw["fallback"]
    lines += ["", "const struct texture_policy_table texture_policy_embedded = {",
              f"\t{raw['schema']}, {raw['policy_version']},",
              f"\t{RESULTS[r['graphic']]}, {RESULTS[r['surface']]}, {RESULTS[r['companion']]}, {RESULTS[r['bump']]},",
              f"\t{float(f['low'])}f, {float(f['high'])}f, {float(f['sigma'])}f, {int(f['iterations'])},",
              f"\t{names}, {groups}, {prefixes},", f"\t{overrides},", f"\t{damage},", f"\t{materials},", f"\t{scales} }};", ""]
    text = "\n".join(lines)
    if not output.is_file() or output.read_text() != text:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text)


if __name__ == "__main__":
    main()
