#!/usr/bin/env python3
"""Prepare the signed latest-release record's input folder after a release is published.

The record tells a build which VISR version is newest on its platform (docs/data-packs.md, "Latest release record").
This tool merges the released platforms into the previous record and never lowers a version. It does not sign:
the data-pack seed stays outside the repository, so the key holder signs the folder it writes with tools/data_pack.py
and lands the envelope on main under docs/site/packs/tester/latest.json.
"""
from __future__ import annotations

import argparse
import base64
import json
from pathlib import Path
import sys
import time
import urllib.error
import urllib.request

if __package__:
    from .data_pack import LATEST_NAME, LATEST_PLATFORMS, LATEST_VERSION, canonical, validate_latest_record
else:
    from data_pack import LATEST_NAME, LATEST_PLATFORMS, LATEST_VERSION, canonical, validate_latest_record

FEED_URL = "https://code.steverice.org/visr/packs/tester/latest.json"
FEED_LIMIT = 16 * 1024


def version_key(version: str) -> tuple[int, int, int]:
    if not LATEST_VERSION.fullmatch(version):
        raise ValueError(f"version must look like 0.6.0: {version}")
    return tuple(int(part) for part in version.split("."))  # type: ignore[return-value]


def merge(previous: dict | None, version: str, platforms: list[str]) -> dict:
    """The record after releasing version on platforms; a platform keeps a higher version it already has."""
    version_key(version)
    unknown = set(platforms) - set(LATEST_PLATFORMS)
    if not platforms or unknown:
        raise ValueError("platforms must be a nonempty subset of " + ", ".join(LATEST_PLATFORMS))
    merged = dict(previous["platforms"]) if previous else {}
    for platform in platforms:
        if platform not in merged or version_key(version) > version_key(merged[platform]):
            merged[platform] = version
    record = {"schema": 1, "platforms": {name: merged[name] for name in LATEST_PLATFORMS if name in merged}}
    validate_latest_record(canonical(record))
    return record


def record_from_envelope(data: bytes) -> dict:
    """The record inside a signed envelope. The signature is not checked here: the key holder signs the successor."""
    envelope = json.loads(data)
    payload = json.loads(base64.b64decode(envelope["payload"], validate=True))
    return validate_latest_record(base64.b64decode(payload["files"][LATEST_NAME], validate=True))


def fetch_previous(url: str) -> dict | None:
    try:
        with urllib.request.urlopen(url, timeout=45) as response:
            return record_from_envelope(response.read(FEED_LIMIT + 1)[:FEED_LIMIT])
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platforms", required=True, help="comma-separated, from the release manifest")
    parser.add_argument("--output", required=True, type=Path, help="folder to write latest-release.json into")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--previous-envelope", type=Path, help="the live latest.json envelope, as a file")
    source.add_argument("--previous-feed", default=FEED_URL, help="where to read the live envelope; 404 means none yet")
    parser.add_argument("--sequence", type=int, help="default: the current Unix time")
    args = parser.parse_args(arguments)
    previous = (record_from_envelope(args.previous_envelope.read_bytes()) if args.previous_envelope
                else fetch_previous(args.previous_feed))
    record = merge(previous, args.version, [name for name in args.platforms.split(",") if name])
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / LATEST_NAME).write_bytes(canonical(record) + b"\n")
    sequence = args.sequence or int(time.time())
    print(f"Wrote {args.output / LATEST_NAME}: {json.dumps(record['platforms'], sort_keys=True)}")
    print(f"Sign with: python3 tools/data_pack.py --kind latest --channel tester --sequence {sequence} "
          f"--compatibility latest-release-1 --input {args.output} --signing-key SEED --output latest.json")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        print(f"latest_release: {error}", file=sys.stderr)
        sys.exit(1)
