"""Focused contract tests for Pages recording-feed overlays."""

from __future__ import annotations

import base64
import json
import urllib.error
from pathlib import Path
from typing import Any

import pytest

import ios_build as workflow_ios_build
from tools import pages_recording_overlay as overlay


def fixture_feed(compatibility: str, sequence: int) -> bytes:
    """Build metadata-only bytes; `checked_feed` is stubbed as the verifier boundary."""
    manifest = json.dumps(
        {"compatibility": compatibility, "sequence": sequence}, sort_keys=True
    ).encode()
    return json.dumps(
        {
            "manifest": base64.b64encode(manifest).decode(),
            "payload": "fixture",
            "signature": "fixture",
        },
        sort_keys=True,
    ).encode()


def configure_overlay(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    *,
    path: str,
    candidate: bytes,
    source_feed: bytes | None = None,
    deployed_feed: bytes | None = None,
    invalid_feeds: set[bytes] | None = None,
) -> tuple[list[tuple[bytes, str]], list[str]]:
    source, commit, blob = "a" * 40, "b" * 40, "c" * 40
    monkeypatch.setattr(overlay, "ROOT", tmp_path)
    monkeypatch.setattr(
        workflow_ios_build,
        "DATA_PACK_FEED",
        "https://example.test/packs/{channel}/{kind}.json",
    )
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    monkeypatch.setenv("GITHUB_EVENT_NAME", "workflow_dispatch")
    monkeypatch.setenv("GITHUB_REF", "refs/heads/main")
    monkeypatch.setenv("GITHUB_SHA", source)
    monkeypatch.setenv("GITHUB_REPOSITORY", "example/visr")
    feed_path = tmp_path / path
    if source_feed is not None:
        feed_path.parent.mkdir(parents=True, exist_ok=True)
        feed_path.write_bytes(source_feed)

    def fake_git(*args: str) -> bytes:
        if args == ("rev-parse", "HEAD"):
            return source.encode()
        if args == ("status", "--porcelain", "--untracked-files=all"):
            return b""
        if args[0] == "fetch":
            return b""
        if args[:3] == ("rev-list", "--parents", "--max-count=1"):
            return f"{commit} {source}".encode()
        if args[0] == "diff":
            return path.encode() + b"\0"
        if args[:2] == ("ls-tree", commit):
            return f"100644 blob {blob} {path}\n".encode()
        if args == ("cat-file", "-s", blob):
            return str(len(candidate)).encode()
        if args == ("cat-file", "blob", blob):
            return candidate
        raise AssertionError(args)

    monkeypatch.setattr(overlay, "git", fake_git)
    verified: list[tuple[bytes, str]] = []
    deployed_urls: list[str] = []

    def verify(data: bytes, expected_guest: str) -> int:
        verified.append((data, expected_guest))
        manifest = json.loads(base64.b64decode(json.loads(data)["manifest"]))
        compatibility = manifest["compatibility"]
        assert compatibility.endswith(":" + expected_guest)
        if invalid_feeds and data in invalid_feeds:
            raise ValueError("fixture signature is invalid")
        return manifest["sequence"]

    class Response:
        def __enter__(self) -> Response:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, _limit: int = -1) -> bytes:
            assert deployed_feed is not None
            return deployed_feed

    class Opener:
        def open(self, url: str, timeout: int) -> Response:
            assert timeout == 45
            deployed_urls.append(url)
            if deployed_feed is None:
                raise urllib.error.HTTPError(url, 404, "Not Found", {}, None)
            return Response()

    monkeypatch.setattr(overlay, "checked_feed", verify)
    monkeypatch.setattr(
        overlay.urllib.request,
        "build_opener",
        lambda *_args: Opener(),
    )
    return verified, deployed_urls


def test_mutable_recordings_path_uses_guest_from_catalog_manifest(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    guest = "a" * 64
    candidate = fixture_feed("benchmark-catalog-1:" + guest, 7)
    verified, urls = configure_overlay(
        monkeypatch,
        tmp_path,
        path="docs/site/packs/tester/recordings.json",
        candidate=candidate,
    )

    assert overlay.overlay("b" * 40) == (
        "docs/site/packs/tester/recordings.json",
        candidate,
        7,
    )
    assert verified == [(candidate, guest)]
    assert urls == ["https://example.test/packs/tester/recordings.json"]


def test_mutable_feed_checks_source_and_deployed_domains_independently(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    old_guest, new_guest = "a" * 64, "b" * 64
    source_feed = fixture_feed("benchmark-catalog-1:" + old_guest, 100)
    deployed_feed = fixture_feed("benchmark-catalog-1:" + new_guest, 8)
    candidate = fixture_feed("benchmark-catalog-1:" + new_guest, 7)
    verified, urls = configure_overlay(
        monkeypatch,
        tmp_path,
        path="docs/site/packs/tester/recordings.json",
        candidate=candidate,
        source_feed=source_feed,
        deployed_feed=deployed_feed,
    )

    with pytest.raises(ValueError, match="higher signed sequence"):
        overlay.overlay("b" * 40)

    assert verified == [
        (candidate, new_guest),
        (source_feed, old_guest),
        (deployed_feed, new_guest),
    ]
    assert urls == ["https://example.test/packs/tester/recordings.json"]


def test_mutable_feed_rejects_source_rollback_in_the_candidate_domain(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    old_guest, guest = "a" * 64, "b" * 64
    source_feed = fixture_feed("benchmark-catalog-1:" + guest, 8)
    deployed_feed = fixture_feed("benchmark-catalog-1:" + old_guest, 100)
    candidate = fixture_feed("benchmark-catalog-1:" + guest, 7)
    verified, _ = configure_overlay(
        monkeypatch,
        tmp_path,
        path="docs/site/packs/tester/recordings.json",
        candidate=candidate,
        source_feed=source_feed,
        deployed_feed=deployed_feed,
    )

    with pytest.raises(ValueError, match="higher signed sequence"):
        overlay.overlay("b" * 40)

    assert verified == [
        (candidate, guest),
        (source_feed, guest),
        (deployed_feed, old_guest),
    ]


def test_mutable_feed_accepts_a_new_signed_guest_domain_after_validating_old_feed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    old_guest, new_guest = "a" * 64, "b" * 64
    old_feed = fixture_feed("benchmark-catalog-1:" + old_guest, 100)
    candidate = fixture_feed("benchmark-catalog-1:" + new_guest, 1)
    verified, _ = configure_overlay(
        monkeypatch,
        tmp_path,
        path="docs/site/packs/tester/recordings.json",
        candidate=candidate,
        source_feed=old_feed,
        deployed_feed=old_feed,
    )

    assert overlay.overlay("b" * 40) == (
        "docs/site/packs/tester/recordings.json",
        candidate,
        1,
    )
    assert verified == [
        (candidate, new_guest),
        (old_feed, old_guest),
        (old_feed, old_guest),
    ]


@pytest.mark.parametrize("invalid_location", ["source", "deployed"])
def test_mutable_feed_does_not_ignore_invalid_old_envelopes(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    invalid_location: str,
) -> None:
    old_guest, new_guest = "a" * 64, "b" * 64
    old_feed = fixture_feed("benchmark-catalog-1:" + old_guest, 100)
    candidate = fixture_feed("benchmark-catalog-1:" + new_guest, 1)
    verified, _ = configure_overlay(
        monkeypatch,
        tmp_path,
        path="docs/site/packs/tester/recordings.json",
        candidate=candidate,
        source_feed=old_feed if invalid_location == "source" else None,
        deployed_feed=old_feed if invalid_location == "deployed" else None,
        invalid_feeds={old_feed},
    )

    with pytest.raises(ValueError, match="signature is invalid"):
        overlay.overlay("b" * 40)

    assert (old_feed, old_guest) in verified


def test_mutable_feed_does_not_ignore_malformed_deployed_envelope(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    old_guest, new_guest = "a" * 64, "b" * 64
    source_feed = fixture_feed("benchmark-catalog-1:" + old_guest, 100)
    candidate = fixture_feed("benchmark-catalog-1:" + new_guest, 1)
    verified, _ = configure_overlay(
        monkeypatch,
        tmp_path,
        path="docs/site/packs/tester/recordings.json",
        candidate=candidate,
        source_feed=source_feed,
        deployed_feed=b"not-json",
    )

    with pytest.raises(ValueError):
        overlay.overlay("b" * 40)

    assert verified == [(candidate, new_guest), (source_feed, old_guest)]


def test_mutable_feed_requires_catalog_compatibility(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    guest = "a" * 64
    candidate = fixture_feed("benchmark-replay-1:" + guest, 1)
    verified, _ = configure_overlay(
        monkeypatch,
        tmp_path,
        path="docs/site/packs/tester/recordings.json",
        candidate=candidate,
    )

    with pytest.raises(ValueError, match="catalog-1"):
        overlay.overlay("b" * 40)

    assert verified == [(candidate, guest)]


def test_guest_specific_overlay_keeps_filename_bound_domain(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    guest = "a" * 64
    path = f"docs/site/packs/tester/recordings-{guest}.json"
    candidate = fixture_feed("benchmark-replay-1:" + guest, 2)
    source_feed = fixture_feed("benchmark-catalog-1:" + guest, 1)
    verified, urls = configure_overlay(
        monkeypatch,
        tmp_path,
        path=path,
        candidate=candidate,
        source_feed=source_feed,
        deployed_feed=source_feed,
    )

    assert overlay.overlay("b" * 40) == (path, candidate, 2)
    assert verified == [
        (candidate, guest),
        (source_feed, guest),
        (source_feed, guest),
    ]
    assert urls == [f"https://example.test/packs/tester/recordings-{guest}.json"]
