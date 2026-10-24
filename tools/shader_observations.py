#!/usr/bin/env python3
"""Validate untrusted ShaderObservation exports and stage descriptors for manual review.

Use --metadata with sanitized CloudKit fields, --asset with the gzip JSON asset,
--guest-sha256 with the approved guest hash, and --output with a private catalog.
Stock aliases require an independently verified --official JSON fingerprint-to-key
catalog. Staging never updates bundled lists or signs a feed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
import zlib

import data_pack

MAX_COMPRESSED = 128 * 1024
MAX_DECODED = 256 * 1024
DUPLICATED = ("schema", "mapFingerprint", "mapKey", "mapFormat", "model", "platform",
              "renderer", "appVersion", "appBuild", "guestSHA256", "descriptorABI", "shaderListID")
FIELDS = set(DUPLICATED) | {"nonce", "descriptors", "dependencies"}
HEX = re.compile(r"[0-9a-f]{64}\Z")
TOKEN = re.compile(r"[A-Za-z0-9][A-Za-z0-9,._:+-]{0,159}\Z")


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def official_maps():
    """Read the same verified retail fingerprint table used by native attribution."""
    header = data_pack.ROOT / "port/ios/host/host_shader_official_maps.h"
    return dict(re.findall(r'\{ "([0-9a-f]{64})", "([a-z0-9_-]+)" \}', header.read_text()))


def pairs_unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def load_json(raw):
    try:
        return json.loads(raw, object_pairs_hook=pairs_unique,
                          parse_constant=lambda value: (_ for _ in ()).throw(ValueError("nonfinite JSON")))
    except (UnicodeError, RecursionError, json.JSONDecodeError) as error:
        raise ValueError("invalid JSON") from error


def decode(asset):
    if not asset or len(asset) > MAX_COMPRESSED:
        raise ValueError("compressed payload limit")
    try:
        decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
        raw = decoder.decompress(asset, MAX_DECODED + 1)
        if len(raw) > MAX_DECODED or decoder.unconsumed_tail or not decoder.eof or decoder.unused_data:
            raise ValueError("invalid gzip stream or decoded payload limit")
        return raw
    except zlib.error as error:
        raise ValueError("invalid gzip stream") from error


def order(line):
    return ({"vs": 0, "ps": 1, "pipeline": 2}.get(line.split(" ")[0], 3), line)


def admit(lines):
    """The publisher and the app share this exact native descriptor validator."""
    with tempfile.TemporaryDirectory(prefix="visr-shader-observations-") as temporary:
        (Path(temporary) / "observed.txt").write_text("".join(line + "\n" for line in sorted(set(lines), key=order)))
        data_pack.validate_shaders(temporary)


def canonical(line):
    if not isinstance(line, str) or len(line) > 639 or not line or not line.isascii():
        return False
    fields = line.split(" ")
    if not all(fields) or any(any(character.isspace() for character in field) for field in fields):
        return False
    try:
        if fields[0] == "vs" and len(fields) == 4:
            return fields[2] == str(int(fields[2])) and fields[3] == format(int(fields[3], 16), "x")
        if fields[0] == "ps" and len(fields) == 3:
            return True
        if fields[0] == "pipeline" and len(fields) == 13:
            vertex = fields[1].split(".")
            return (len(vertex) == 3 and vertex[1] == str(int(vertex[1])) and
                    vertex[2] == format(int(vertex[2], 16), "x") and
                    all(fields[index] == str(int(fields[index])) for index in (*range(3, 10), 11, 12)))
    except ValueError:
        pass
    return False


def validate(asset, metadata, *, guest_sha256, descriptor_abi="shader-lists-2", official=None):
    raw = decode(asset)
    payload_hash = digest(raw)
    value = load_json(raw)
    if not isinstance(value, dict) or set(value) != FIELDS:
        raise ValueError("payload schema fields")
    if type(value["schema"]) is not int or value["schema"] != 1:
        raise ValueError("unsupported schema")
    if not isinstance(metadata, dict):
        raise ValueError("metadata schema")
    for key in DUPLICATED:
        if type(metadata.get(key)) is not type(value[key]) or metadata.get(key) != value[key]:
            raise ValueError("duplicated metadata mismatch")
    if metadata.get("payloadSHA256") != payload_hash or metadata.get("recordName") != "shader-" + payload_hash:
        raise ValueError("payload hash or record identity mismatch")
    for key in set(DUPLICATED) - {"schema"}:
        if not isinstance(value[key], str) or not TOKEN.fullmatch(value[key]):
            raise ValueError("invalid identity field")
    if (not HEX.fullmatch(value["mapFingerprint"]) or not HEX.fullmatch(value["guestSHA256"]) or
            value["guestSHA256"] != guest_sha256 or value["descriptorABI"] != descriptor_abi or
            value["renderer"] != "metal" or value["platform"] not in {"ios", "tvos", "visionos", "macos"} or
            not re.fullmatch(r"cache-(0|[1-9][0-9]{0,9})", value["mapFormat"]) or
            int(value["mapFormat"][6:]) > 4294967295 or
            not isinstance(value["nonce"], str) or not re.fullmatch(r"[0-9a-f]{32}", value["nonce"]) or
            not re.fullmatch(r"shader-lists-2:(bundled|0|[1-9][0-9]{0,19}):[0-9a-f]{64}", value["shaderListID"])):
        raise ValueError("unsupported identity")
    fingerprint = value["mapFingerprint"]
    stock = (official_maps() if official is None else official).get(fingerprint)
    if stock and not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,62}", stock):
        raise ValueError("invalid verified stock catalog")
    community = "m_" + fingerprint[:60]
    if value["mapKey"] != (stock or community):
        raise ValueError("unverified map alias")
    for key, maximum in (("descriptors", 256), ("dependencies", 512)):
        lines = value[key]
        if (not isinstance(lines, list) or len(lines) > maximum or not all(canonical(line) for line in lines)
                or lines != sorted(set(lines))):
            raise ValueError("noncanonical descriptor list")
    if (not value["descriptors"] or set(value["descriptors"]) & set(value["dependencies"]) or
            any(line.startswith("pipeline ") for line in value["dependencies"]) or
            type(metadata.get("descriptorCount")) is not int or metadata["descriptorCount"] != len(value["descriptors"])):
        raise ValueError("invalid descriptor membership")
    # Supporting records must be actual dependencies of contributed pipelines.
    referenced = set()
    for line in value["descriptors"]:
        fields = line.split()
        if fields[0] == "pipeline":
            referenced.update(("vs " + fields[1], "ps " + fields[2]))
    for line in value["dependencies"]:
        fields = line.split()
        key = "ps " + fields[1] if fields[0] == "ps" else "vs " + fields[1] + "." + fields[2] + "." + fields[3]
        if key not in referenced:
            raise ValueError("unrelated dependency")
    admit(value["descriptors"] + value["dependencies"])
    return {"mapFingerprint": fingerprint, "mapKey": value["mapKey"], "descriptorABI": descriptor_abi,
            "renderer": "metal", "category": "stock" if stock else "community",
            "descriptors": value["descriptors"], "dependencies": value["dependencies"]}


def atomic_write(path, raw):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        output.write(raw)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def quarantine(output, identity, reason):
    # Never copy arbitrary payload text or CloudKit creator metadata into exports.
    name = digest(identity.encode())
    atomic_write(Path(output) / "quarantine" / (name + ".json"),
                 json.dumps({"rejected": name, "reason": reason}, sort_keys=True).encode())


def stage(clean, output):
    output = Path(output)
    key = clean["mapKey"]
    target = output / clean["category"] / (key + ".txt")
    metadata_path = target.with_suffix(".json")
    try:
        if target.exists() != metadata_path.exists():
            raise ValueError("incomplete catalog state requires review")
        evidence = {name: clean[name] for name in ("mapFingerprint", "mapKey", "descriptorABI", "renderer")}
        misses = {digest(line.encode()) for line in clean["descriptors"]}
        lines = set(clean["descriptors"] + clean["dependencies"])
        if metadata_path.exists():
            previous = load_json(metadata_path.read_bytes())
            if any(previous.get(name) != value for name, value in evidence.items()):
                raise ValueError("catalog identity or fingerprint prefix collision")
            if previous.get("listSHA256") != digest(bounded_read(target, 4 * 1024 * 1024)):
                raise ValueError("incomplete catalog replacement requires review")
            misses.update(previous["misses"])
        if target.exists():
            lines.update(target.read_text().splitlines())
        admit(lines)
        text = "".join(line + "\n" for line in sorted(lines, key=order))
        if len(text.encode()) > 4 * 1024 * 1024:
            raise ValueError("catalog map limit")
        evidence["misses"] = sorted(misses)
        evidence["listSHA256"] = digest(text.encode())
        atomic_write(target, text.encode())
        atomic_write(metadata_path, json.dumps(evidence, sort_keys=True, separators=(",", ":")).encode())
    except ValueError:
        quarantine(output, clean["mapFingerprint"], "catalog conflict or admission failure")
        raise
    return target


def bounded_read(path, limit):
    with Path(path).open("rb") as source:
        raw = source.read(limit + 1)
    if len(raw) > limit:
        raise ValueError("input file limit")
    return raw


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--asset", required=True, type=Path)
    parser.add_argument("--guest-sha256", required=True)
    parser.add_argument("--descriptor-abi", default="shader-lists-2")
    parser.add_argument("--official", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        official = load_json(bounded_read(args.official, 65536)) if args.official else official_maps()
        if not isinstance(official, dict):
            raise ValueError("stock catalog schema")
        clean = validate(bounded_read(args.asset, MAX_COMPRESSED), load_json(bounded_read(args.metadata, 8192)),
                         guest_sha256=args.guest_sha256, descriptor_abi=args.descriptor_abi, official=official)
        stage(clean, args.output)
    except (ValueError, OSError):
        quarantine(args.output, str(args.metadata), "record rejected")
        parser.exit(1, "shader observation rejected; see quarantine\n")


if __name__ == "__main__":
    main()
