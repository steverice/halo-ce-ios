#!/usr/bin/env python3
"""Stage the main site with one reviewed, signed recording feed from a data commit."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
FEED_PATH = re.compile(r"docs/site/packs/tester/recordings-([0-9a-f]{64})\.json")


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=ROOT)


def checked_feed(data: bytes, guest: str) -> int:
    # Import only the checked-out main code, never anything from the data commit.
    if __package__:
        from .release_validation import packs, verify_pages_recording_feed
    else:
        from release_validation import packs, verify_pages_recording_feed

    verify_pages_recording_feed(data, guest)
    envelope = packs.decode_json(data)
    return packs.decode_json(packs.decode_base64(envelope["manifest"], 65536))["sequence"]


def overlay(commit: str) -> tuple[str, bytes, int]:
    from data_pack import ENVELOPE_LIMITS
    from ios_build import DATA_PACK_FEED
    from release_validation import NoRedirect

    source = os.environ["GITHUB_SHA"]
    repository = os.environ["GITHUB_REPOSITORY"]
    if (os.environ.get("GITHUB_ACTIONS") != "true"
            or os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch"
            or os.environ.get("GITHUB_REF") != "refs/heads/main"):
        raise ValueError("Recording overlays require workflow_dispatch from main")
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or not re.fullmatch(r"[0-9a-f]{40}", source):
        raise ValueError("Source and recording commits must be exact full SHAs")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Invalid workflow repository")
    if git("rev-parse", "HEAD").decode().strip() != source:
        raise ValueError("Site checkout differs from the workflow source")
    if git("status", "--porcelain", "--untracked-files=all"):
        raise ValueError("Site source checkout must be clean")
    git("fetch", "--no-tags", "--depth=2", "--no-write-fetch-head",
        f"https://github.com/{repository}.git", commit)
    if git("rev-list", "--parents", "--max-count=1", commit).decode().split() != [commit, source]:
        raise ValueError("Recording commit must have exactly the workflow source as its parent")
    paths = git("diff", "--name-only", "--no-renames", "-z", source, commit, "--").split(b"\0")
    if len(paths) != 2 or paths[-1] != b"":
        raise ValueError("Recording commit must change exactly one feed file")
    path = paths[0].decode("ascii")
    match = FEED_PATH.fullmatch(path)
    if not match:
        raise ValueError("Only a guest-specific tester recording JSON may be overlaid")
    entry = git("ls-tree", commit, "--", path).decode().split()
    if len(entry) != 4 or entry[:2] != ["100644", "blob"]:
        raise ValueError("Recording feed must be a regular, nonexecutable file")
    limit = ENVELOPE_LIMITS["recordings"]
    size = int(git("cat-file", "-s", entry[2]))
    if not 0 < size <= limit:
        raise ValueError("Recording envelope exceeds its bounded download size")
    data = git("cat-file", "blob", entry[2])
    if len(data) != size:
        raise ValueError("Recording blob size changed")
    guest = match[1]
    sequence = checked_feed(data, guest)
    previous = []
    existing = ROOT / path
    if existing.exists():
        if existing.is_symlink() or existing.stat().st_size > limit:
            raise ValueError("Existing source feed is not a bounded regular file")
        previous.append(existing.read_bytes())
    # Also prevent rollback of an earlier overlay not yet persisted to main.
    url = DATA_PACK_FEED.format(channel="tester", kind="recordings-" + guest)
    try:
        with urllib.request.build_opener(NoRedirect()).open(url, timeout=45) as response:
            deployed = response.read(limit + 1)
        if len(deployed) > limit:
            raise ValueError("Existing deployed feed exceeds its download limit")
        previous.append(deployed)
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
    for old in previous:
        old_sequence = checked_feed(old, guest)
        if old != data and sequence <= old_sequence:
            raise ValueError("A replacement recording feed must have a higher signed sequence")
    return path, data, sequence


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recording-commit", default="")
    args = parser.parse_args()
    selected = overlay(args.recording_commit) if args.recording_commit else None
    destination = ROOT / "build/pages"
    shutil.copytree(ROOT / "docs/site", destination)
    if selected:
        path, data, sequence = selected
        target = destination / Path(path).relative_to("docs/site")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        receipt = {"source_sha": os.environ["GITHUB_SHA"], "recording_commit": args.recording_commit,
                   "path": path, "sha256": hashlib.sha256(data).hexdigest(), "sequence": sequence}
        (ROOT / "build/pages-recording-overlay.json").write_text(json.dumps(receipt, indent=2))
        print(json.dumps(receipt))


if __name__ == "__main__":
    main()
