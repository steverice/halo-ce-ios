#!/usr/bin/env python3
"""Stage reviewed signed feeds without rolling back prior Pages data updates.

The publication receipt is for the publisher alone. It only names bounded signed
asset paths and hashes; the app continues to request its individual feed URLs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile
import urllib.error
import urllib.request
from typing import Literal, overload

if __package__:
    from . import pages_recording_overlay as legacy
    from . import release_validation as validation
    from .data_pack import ENVELOPE_LIMITS, MAXIMUM_SEQUENCE, profile_target_parts
else:
    import pages_recording_overlay as legacy
    import release_validation as validation
    from data_pack import ENVELOPE_LIMITS, MAXIMUM_SEQUENCE, profile_target_parts

ROOT = Path(__file__).resolve().parents[1]
SITE_URL = "https://code.steverice.org/visr"
PREFIX = "docs/site/packs/tester/"
RECEIPT = PREFIX + "publication.json"
MARKER = PREFIX + "publication-format.txt"
MARKER_DATA = b"visr-signed-assets-1\n"
RECORDING = PREFIX + "recordings.json"
SHADER = PREFIX + "shaders.json"
LEGACY_PROFILE_INDEX = PREFIX + "profiles-2.json"
MAX_ASSETS = 256
MAX_RECEIPT = 65536
MAX_TOTAL = 512 * 1024 * 1024


def allowed_asset(path: str) -> tuple[str, str | None]:
    if not isinstance(path, str) or not path.startswith(PREFIX):
        raise ValueError("Only tester signed asset paths may be overlaid")
    if path == RECORDING or legacy.FEED_PATH.fullmatch(path):
        return "recording", None
    if path == SHADER:
        return "shader", None
    if re.fullmatch(re.escape(PREFIX) + r"shader-maps/[0-9a-f]{64}\.zlib", path):
        return "shader-map", None
    profile = re.fullmatch(re.escape(PREFIX) + r"profiles/(.+)\.json", path)
    if profile:
        target = profile[1]
        profile_target_parts(target)
        return "profile", target
    raise ValueError(
        "Only canonical profile, shader and recording feeds may be overlaid"
    )


def limit_for(path: str) -> int:
    kind, _ = allowed_asset(path)
    if kind == "shader-map":
        return validation.packs.data_pack.SHADER_COMPRESSED_FILE_LIMIT
    return ENVELOPE_LIMITS[
        {"profile": "profiles", "shader": "shaders", "recording": "recordings"}[kind]
    ]


def verify_asset(path: str, data: bytes) -> dict:
    kind, target = allowed_asset(path)
    if not 0 < len(data) <= limit_for(path):
        raise ValueError("Asset envelope exceeds its bounded size")
    digest = hashlib.sha256(data).hexdigest()
    if kind == "shader-map":
        if path != PREFIX + "shader-maps/" + digest + ".zlib":
            raise ValueError("Shader asset filename differs from its content hash")
        return {"compatibility": "shader-map", "sequence": 1}
    if kind == "recording":
        domain, guest, sequence = legacy.mutable_recording_domain(
            data, require_catalog=path == RECORDING
        )
        match = legacy.FEED_PATH.fullmatch(path)
        if match and match[1] != guest:
            raise ValueError("Recording filename differs from signed guest domain")
        return {"compatibility": domain, "sequence": sequence}
    if kind == "profile":
        assert target is not None
        validation.verify_feed(
            data,
            kind,
            {"profile_feed_sha256s": {target: digest}},
            profile_target=target,
        )
        domain = "device-profiles-1:" + target
    else:
        metadata = validation.verify_feed(data, kind, {"shader_feed_sha256": digest}, shader_index_only=True)
        if metadata:
            return metadata
        domain = "shader-lists-1"
    envelope = validation.packs.decode_json(data)
    manifest = validation.packs.decode_json(
        validation.packs.decode_base64(envelope["manifest"], 65536)
    )
    return {"compatibility": domain, "sequence": manifest["sequence"]}


def verify_shader_bundle(assets: dict[str, bytes]) -> None:
    """Only an authenticated index may admit compressed map bytes into publication."""
    maps = {path: raw for path, raw in assets.items() if path.startswith(PREFIX + "shader-maps/")}
    if SHADER not in assets:
        if maps:
            raise ValueError("Shader assets require their signed index")
        return
    metadata = verify_asset(SHADER, assets[SHADER])
    if metadata["compatibility"] != "shader-lists-2":
        if maps:
            raise ValueError("Legacy shader feed cannot authenticate compressed assets")
        return
    envelope = validation.packs.decode_json(assets[SHADER])
    payload = validation.packs.decode_base64(envelope["payload"], validation.packs.data_pack.SHADER_INDEX_LIMIT)
    index = validation.packs.shader_index(payload)
    expected = {PREFIX + "shader-maps/" + descriptor[2] + ".zlib" for descriptor in index.values()}
    if set(maps) != expected:
        raise ValueError("Published shader assets differ from the signed index")
    validation.verify_feed(assets[SHADER], "shader", {"shader_feed_sha256": hashlib.sha256(assets[SHADER]).hexdigest()},
        shader_assets={Path(path).name: raw for path, raw in maps.items()})


def parse_receipt(data: bytes) -> dict[str, str]:
    if not 0 < len(data) <= MAX_RECEIPT:
        raise ValueError("Publication receipt exceeds its bounded size")

    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Publication receipt contains duplicate keys")
            result[key] = value
        return result

    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=unique)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("Publication receipt is invalid JSON") from error
    if (
        not isinstance(value, dict)
        or set(value) != {"schema", "assets"}
        or type(value["schema"]) is not int
        or value["schema"] != 1
    ):
        raise ValueError("Publication receipt has invalid fields")
    assets = value["assets"]
    if (
        not isinstance(assets, dict)
        or not 1 <= len(assets) <= MAX_ASSETS
        or RECORDING not in assets
    ):
        raise ValueError("Publication receipt must retain the known recording feed")
    for path, digest in assets.items():
        allowed_asset(path)
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Publication receipt contains an invalid asset hash")
    return assets


@overload
def fetch(path: str, maximum: int, *, optional: Literal[False] = False) -> bytes: ...


@overload
def fetch(path: str, maximum: int, *, optional: Literal[True]) -> bytes | None: ...


def fetch(path: str, maximum: int, *, optional: bool = False) -> bytes | None:
    # The sole generated metadata endpoint is not an allowed overlay path.
    if path not in {RECEIPT, MARKER}:
        allowed_asset(path)
    url = SITE_URL + "/" + path.removeprefix("docs/site/")
    if not url.startswith("https://"):
        raise ValueError("Published asset downloads require HTTPS")
    try:
        with urllib.request.build_opener(validation.NoRedirect()).open(
            url, timeout=45
        ) as response:
            data = response.read(maximum + 1)
    except urllib.error.HTTPError as error:
        if optional and error.code == 404:
            return None
        raise ValueError("Required published asset is unavailable: " + path) from error
    if not 0 < len(data) <= maximum:
        raise ValueError("Published asset exceeds its bounded size")
    return data


def check_budget(assets: dict[str, bytes]) -> None:
    if not 1 <= len(assets) <= MAX_ASSETS or sum(map(len, assets.values())) > MAX_TOTAL:
        raise ValueError("Publication assets exceed their bounded total size")


def live_assets() -> dict[str, bytes]:
    receipt = fetch(RECEIPT, MAX_RECEIPT, optional=True)
    marker = fetch(MARKER, len(MARKER_DATA), optional=True)
    if receipt is None:
        # The only supported pre-receipt deployment already has recordings.json.
        if marker is not None:
            raise ValueError(
                "Publication receipt is missing for deployed signed assets"
            )
        raw = fetch(RECORDING, limit_for(RECORDING))
        verify_asset(RECORDING, raw)
        return {RECORDING: raw}
    if marker != MARKER_DATA:
        raise ValueError("Publication format marker is missing or invalid")
    result = {}
    for path, digest in parse_receipt(receipt).items():
        raw = fetch(path, limit_for(path))
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError("Published envelope differs from receipt hash: " + path)
        verify_asset(path, raw)
        result[path] = raw
        check_budget(result)
    verify_shader_bundle(result)
    return result


def trusted_source() -> str:
    source = os.environ.get("GITHUB_SHA", "")
    if (
        os.environ.get("GITHUB_ACTIONS") != "true"
        or os.environ.get("GITHUB_REF") != "refs/heads/main"
        or os.environ.get("GITHUB_EVENT_NAME") not in {"push", "workflow_dispatch"}
        or not re.fullmatch(r"[0-9a-f]{40}", source)
    ):
        raise ValueError("Asset staging requires the trusted main workflow")
    if legacy.git("rev-parse", "HEAD").decode().strip() != source or legacy.git(
        "status", "--porcelain", "--untracked-files=all"
    ):
        raise ValueError("Site source must be the exact clean workflow checkout")
    return source


def read_source(path: Path, maximum: int) -> bytes:
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= maximum:
        raise ValueError("Asset must be a bounded regular file")
    data = path.read_bytes()
    if len(data) != info.st_size:
        raise ValueError("Asset changed while reading")
    return data


def source_assets() -> dict[str, bytes]:
    result = {}
    directory = ROOT / PREFIX
    for path in sorted(directory.rglob("*")):
        name = path.relative_to(ROOT).as_posix()
        if (
            name == RECORDING
            or name == SHADER
            or legacy.FEED_PATH.fullmatch(name)
            or (name.startswith(PREFIX + "profiles/") and path.is_file())
            or (name.startswith(PREFIX + "shader-maps/") and path.is_file())
        ):
            result[name] = read_source(path, limit_for(name))
            verify_asset(name, result[name])
            check_budget(result)
    verify_shader_bundle(result)
    return result


def checked_in_assets() -> dict[str, bytes]:
    """Return the exact signed feeds checked into the current main site."""
    directory = ROOT / PREFIX
    if not directory.is_dir() or directory.is_symlink():
        raise ValueError("Checked-in tester asset directory is missing or invalid")

    marker = read_source(ROOT / MARKER, len(MARKER_DATA))
    if marker != MARKER_DATA:
        raise ValueError("Checked-in publication format marker is missing or invalid")
    receipt_raw = read_source(ROOT / RECEIPT, MAX_RECEIPT)
    receipt = parse_receipt(receipt_raw)
    if SHADER not in receipt:
        raise ValueError("Publication receipt must retain the shader feed")

    assets = {}
    found_paths = set()
    for current, directories, files in os.walk(directory, followlinks=False):
        parent = Path(current)
        for name in directories:
            child = parent / name
            if child.is_symlink():
                raise ValueError("Tester asset directories cannot be symbolic links")
            relative = child.relative_to(ROOT).as_posix()
            if relative not in {PREFIX + "profiles", PREFIX + "shader-maps"} and not relative.startswith(
                PREFIX + "profiles/"
            ):
                raise ValueError("Unexpected directory under the tester asset root")
        for name in files:
            path = parent / name
            relative = path.relative_to(ROOT).as_posix()
            if path.is_symlink():
                raise ValueError("Tester assets cannot be symbolic links")
            if relative in {RECEIPT, MARKER, LEGACY_PROFILE_INDEX}:
                continue
            if not relative.endswith((".json", ".zlib")):
                raise ValueError("Unexpected file under the tester asset root")
            try:
                allowed_asset(relative)
            except ValueError as error:
                raise ValueError(
                    "Tester contains a path that is not an allowed signed asset path"
                ) from error
            found_paths.add(relative)
            assets[relative] = read_source(path, limit_for(relative))

    if found_paths != set(receipt):
        raise ValueError("Checked-in signed asset paths do not match the receipt")
    for path, data in assets.items():
        if hashlib.sha256(data).hexdigest() != receipt[path]:
            raise ValueError("Checked-in asset differs from its receipt hash: " + path)
        metadata = verify_asset(path, data)
        sequence = metadata.get("sequence")
        if type(sequence) is not int or not 1 <= sequence <= MAXIMUM_SEQUENCE:
            raise ValueError("Checked-in asset has an invalid signed sequence: " + path)
    check_budget(assets)
    verify_shader_bundle(assets)
    return assets


def stage_main_site(*, destination: Path | None = None) -> Path:
    """Validate and stage only the site and signed feeds checked into main."""
    trusted_source()
    checked_in_assets()
    destination = destination or ROOT / "build/pages"
    if destination.exists():
        raise ValueError("Staging destination already exists")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".pages-main-assets-", dir=destination.parent))
    try:
        shutil.copytree(ROOT / "docs/site", temporary, dirs_exist_ok=True)
        os.replace(temporary, destination)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return destination


def candidate_assets(commit: str) -> dict[str, bytes]:
    source = trusted_source()
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    if (
        os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch"
        or not re.fullmatch(r"[0-9a-f]{40}", commit)
        or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository)
    ):
        raise ValueError("Asset overlays require an exact reviewed manual data commit")
    legacy.git(
        "fetch",
        "--no-tags",
        "--depth=2",
        "--no-write-fetch-head",
        f"https://github.com/{repository}.git",
        commit,
    )
    if legacy.git(
        "rev-list", "--parents", "--max-count=1", commit
    ).decode().split() != [commit, source]:
        raise ValueError(
            "Asset commit must have exactly the workflow main as its parent"
        )
    paths = legacy.git(
        "diff", "--name-only", "--no-renames", "-z", source, commit, "--"
    ).split(b"\0")
    if not paths or paths[-1] != b"" or not 1 <= len(paths) - 1 <= MAX_ASSETS:
        raise ValueError("Asset commit has invalid path count")
    result = {}
    for encoded in paths[:-1]:
        path = encoded.decode("ascii")
        if path in result:
            raise ValueError("Asset commit repeats a path")
        maximum = limit_for(path)
        entry = legacy.git("ls-tree", commit, "--", path).decode().split()
        if len(entry) != 4 or entry[:2] != ["100644", "blob"] or entry[3] != path:
            raise ValueError("Asset commit must contain regular nonexecutable feeds")
        size = int(legacy.git("cat-file", "-s", entry[2]))
        if not 0 < size <= maximum:
            raise ValueError("Asset envelope exceeds bounded size")
        raw = legacy.git("cat-file", "blob", entry[2])
        if len(raw) != size:
            raise ValueError("Asset blob size changed")
        verify_asset(path, raw)
        result[path] = raw
        check_budget(result)
    return result


def advance(path: str, old: bytes, new: bytes) -> None:
    a, b = verify_asset(path, old), verify_asset(path, new)
    if (
        old != new
        and a["compatibility"] == b["compatibility"]
        and b["sequence"] <= a["sequence"]
    ):
        raise ValueError("Replacement asset needs a higher signed sequence")


def stage_site(
    *,
    asset_commit: str = "",
    recording_commit: str = "",
    destination: Path | None = None,
) -> Path:
    trusted_source()
    if asset_commit and recording_commit:
        raise ValueError("Select one reviewed data commit")
    source = source_assets()
    live = live_assets()
    selected = dict(source)
    for path, raw in live.items():
        if path in source:
            a, b = verify_asset(path, source[path]), verify_asset(path, raw)
            if a["compatibility"] == b["compatibility"]:
                if a["sequence"] > b["sequence"]:
                    continue
                advance(path, source[path], raw)
        selected[path] = raw
    candidates = candidate_assets(asset_commit) if asset_commit else {}
    if recording_commit:
        path, raw, _ = legacy.overlay(recording_commit)
        candidates = {path: raw}
    for path, raw in candidates.items():
        verify_asset(path, raw)
        for previous in (source, live):
            if path in previous:
                advance(path, previous[path], raw)
        selected[path] = raw
    # Drop old immutable maps only after the selected index is known. Deployments
    # stage the complete set together; a new index never references partial data.
    if SHADER in selected and verify_asset(SHADER, selected[SHADER])["compatibility"] == "shader-lists-2":
        envelope = validation.packs.decode_json(selected[SHADER])
        index = validation.packs.shader_index(validation.packs.decode_base64(envelope["payload"], 65536))
        keep = {PREFIX + "shader-maps/" + item[2] + ".zlib" for item in index.values()}
        selected = {path: raw for path, raw in selected.items() if not path.startswith(PREFIX + "shader-maps/") or path in keep}
    check_budget(selected)
    verify_shader_bundle(selected)
    if RECORDING not in selected:
        raise ValueError("Publication must preserve the known recording feed")
    receipt = (
        json.dumps(
            {
                "schema": 1,
                "assets": {
                    path: hashlib.sha256(raw).hexdigest()
                    for path, raw in sorted(selected.items())
                },
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
        + b"\n"
    )
    parse_receipt(receipt)
    destination = destination or ROOT / "build/pages"
    if destination.exists():
        raise ValueError("Staging destination already exists")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".pages-assets-", dir=destination.parent))
    try:
        shutil.copytree(ROOT / "docs/site", temporary, dirs_exist_ok=True)
        copied_maps = temporary / "packs/tester/shader-maps"
        if copied_maps.exists():
            shutil.rmtree(copied_maps)
        for path, raw in selected.items():
            target = temporary / path.removeprefix("docs/site/")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)
        target = temporary / RECEIPT.removeprefix("docs/site/")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(receipt)
        (temporary / MARKER.removeprefix("docs/site/")).write_bytes(MARKER_DATA)
        os.replace(temporary, destination)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    result = stage_main_site()
    receipt = (result / "packs/tester/publication.json").read_bytes()
    print(
        json.dumps(
            {
                "source_sha": os.environ["GITHUB_SHA"],
                "receipt_sha256": hashlib.sha256(receipt).hexdigest(),
                "assets": len(parse_receipt(receipt)),
                "checked_in_main_assets": True,
            }
        )
    )


if __name__ == "__main__":
    main()
