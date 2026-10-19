"""Validate downloaded envelopes using the app contract and publisher snapshot validators."""

from __future__ import annotations

import base64
import json
import re
import tempfile
from pathlib import Path
from typing import Any

if __package__:
    from . import data_pack
else:
    import data_pack


def decode_json(value: bytes) -> dict[str, Any]:
    # PackJSON rejects duplicate keys at every depth, including escaped equivalents.
    def unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        if len(pairs) > 160:
            raise ValueError("Pack JSON object exceeds its key limit")
        result: dict[str, Any] = {}
        for name, item in pairs:
            if name in result:
                raise ValueError("Pack JSON contains duplicate keys")
            result[name] = item
        return result

    def reject_constant(value: str) -> Any:
        raise ValueError("Pack JSON contains a non-JSON numeric constant: " + value)

    result = json.loads(
        value.decode("utf-8"), object_pairs_hook=unique, parse_constant=reject_constant
    )

    def shape(item: Any, depth: int) -> None:
        if isinstance(item, list):
            raise TypeError("Pack JSON must not contain arrays")
        if isinstance(item, dict):
            if depth >= 2:
                raise ValueError("Pack JSON object nesting exceeds its limit")
            for child in item.values():
                shape(child, depth + 1)

    shape(result, 0)
    if not isinstance(result, dict):
        raise TypeError("Pack JSON must be an object")
    return result


def decode_base64(value: Any, maximum: int) -> bytes:
    # Match host_data_pack.m PackBase64, including canonical padding and encoded bounds.
    if not isinstance(value, str) or len(value) > ((maximum + 2) // 3) * 4:
        raise ValueError("Pack base64 value exceeds its limit or has the wrong type")
    decoded = base64.b64decode(value, validate=True)
    if len(decoded) > maximum or base64.b64encode(decoded).decode("ascii") != value:
        raise ValueError("Pack base64 value is not canonical or exceeds its limit")
    return decoded


def validate_manifest(
    manifest: dict[str, Any], kind: str, channel: str, compatibility: str
) -> None:
    # Keep these fields and types aligned with host_data_pack.m verifyEnvelope.
    fields = {
        "schema",
        "kind",
        "channel",
        "sequence",
        "minimum_engine",
        "maximum_engine",
        "compatibility",
        "sha256",
        "notes",
    }
    if set(manifest) != fields:
        raise ValueError("Pack manifest has invalid fields")
    for name, maximum in [
        ("schema", 1),
        ("minimum_engine", 1),
        ("maximum_engine", 1),
        ("sequence", data_pack.MAXIMUM_SEQUENCE),
    ]:
        value = manifest[name]
        if type(value) is not int or not 1 <= value <= maximum:
            raise ValueError("Pack manifest has an invalid integer: " + name)
    if (
        manifest["kind"] != kind
        or manifest["channel"] != channel
        or manifest["compatibility"] != compatibility
    ):
        raise ValueError(
            "Pack manifest compatibility, kind, or channel differs from this build"
        )
    if not isinstance(manifest["sha256"], str) or not re.fullmatch(
        r"[0-9a-f]{64}", manifest["sha256"]
    ):
        raise ValueError("Pack manifest payload hash is invalid")
    notes = manifest["notes"]
    if not isinstance(notes, str) or len(notes.encode("utf-16-le")) // 2 > 4096:
        raise ValueError(
            "Pack manifest notes exceed the app UTF-16 limit or have the wrong type"
        )


def validate_payload(payload: bytes, kind: str, compatibility: str) -> dict[str, bytes]:
    decoded = decode_json(payload)
    if set(decoded) != {"files"} or not isinstance(decoded["files"], dict):
        raise ValueError("Pack payload has invalid fields")
    encoded = decoded["files"]
    maximum_files, maximum_file, maximum_total = data_pack.snapshot_limits(kind)
    if not 1 <= len(encoded) <= maximum_files:
        raise ValueError("Pack file count exceeds its limit")
    total = 0
    with tempfile.TemporaryDirectory(prefix="visr-release-pack-") as temporary:
        folder = Path(temporary)
        for name, value in encoded.items():
            if not data_pack.snapshot_name_allowed(name, kind, compatibility):
                raise ValueError("Pack file name is not allowed")
            content = decode_base64(value, maximum_file)
            total += len(content)
            if total > maximum_total:
                raise ValueError("Pack total file size exceeds its limit")
            destination = folder / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)
        # Reuse the publisher's exact profile/catalog and native recording validators.
        return data_pack.snapshot(folder, kind, compatibility)
