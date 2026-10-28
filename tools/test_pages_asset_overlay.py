"""Pages data updates remain signed, path-bound and durable across deployments."""

from email.message import Message
import hashlib
import json

import pytest

from tools import pages_asset_overlay as overlay

PROFILE = "docs/site/packs/tester/profiles/ios/iPhone18,1/_.json"
PROFILE_2 = "docs/site/packs/tester/device-profiles-2/ios/iPhone18,1/_.json"
SHADER = "docs/site/packs/tester/shaders.json"
RECORDING = "docs/site/packs/tester/recordings.json"


def test_only_signed_asset_paths_are_admitted():
    assert overlay.allowed_asset(PROFILE) == ("profile", "ios/iPhone18,1/_")
    assert overlay.allowed_asset(PROFILE_2) == ("profile", "ios/iPhone18,1/_")
    assert overlay.allowed_asset(SHADER) == ("shader", None)
    assert overlay.allowed_asset(RECORDING) == ("recording", None)
    for path in [
        "docs/site/index.html",
        "docs/site/packs/tester/publication.json",
        "docs/site/packs/stable/shaders.json",
        PROFILE.replace("iPhone18,1", ".."),
        PROFILE.replace("/_", "/Apple-M4"),
        PROFILE.replace("/profiles/", "/device-profiles-3/"),
        PROFILE.replace("/profiles/", "/shader-maps/"),
    ]:
        with pytest.raises(ValueError):
            overlay.allowed_asset(path)


def test_receipt_rejects_unknown_fields_duplicates_hashes_and_paths():
    good = {"schema": 1, "assets": {RECORDING: "a" * 64}}
    assert overlay.parse_receipt(json.dumps(good).encode()) == good["assets"]
    for bad in [
        b'{"schema":1,"schema":1,"assets":{}}',
        json.dumps({**good, "command": "run"}).encode(),
        json.dumps({"schema": 1, "assets": {RECORDING: "bad"}}).encode(),
        json.dumps(
            {"schema": 1, "assets": {"docs/site/index.html": "a" * 64}}
        ).encode(),
        json.dumps({"schema": 1, "assets": {}}).encode(),
    ]:
        with pytest.raises(ValueError):
            overlay.parse_receipt(bad)


def envelope(domain, sequence, tag):
    return json.dumps(
        {"compatibility": domain, "sequence": sequence, "tag": tag}
    ).encode()


def test_successive_independent_updates_survive_normal_source_deployment(
    tmp_path, monkeypatch
):
    (tmp_path / "docs/site").mkdir(parents=True)
    (tmp_path / "docs/site/index.html").write_text("reviewed main site")
    monkeypatch.setattr(overlay, "ROOT", tmp_path)
    monkeypatch.setattr(overlay, "trusted_source", lambda: "a" * 40)
    monkeypatch.setattr(overlay, "source_assets", lambda: {})
    monkeypatch.setattr(overlay, "verify_asset", lambda path, data: json.loads(data))
    live = {
        PROFILE: envelope("device-profiles-1:ios/iPhone18,1/_", 1, "p1"),
        SHADER: envelope("shader-lists-1", 1, "s1"),
        RECORDING: envelope("benchmark-catalog-1:" + "a" * 64, 1, "r1"),
    }
    initial_source = dict(live)
    monkeypatch.setattr(overlay, "source_assets", lambda: dict(initial_source))
    monkeypatch.setattr(overlay, "live_assets", lambda: dict(live))
    candidate = {}
    monkeypatch.setattr(overlay, "candidate_assets", lambda commit: dict(candidate))
    for i, (path, domain) in enumerate(
        [
            (PROFILE, "device-profiles-1:ios/iPhone18,1/_"),
            (SHADER, "shader-lists-1"),
            (RECORDING, "benchmark-catalog-1:" + "b" * 64),
        ],
        1,
    ):
        candidate.clear()
        candidate[path] = envelope(domain, 2, "update" + str(i))
        result = overlay.stage_site(
            asset_commit="b" * 40, destination=tmp_path / ("stage" + str(i))
        )
        live[path] = candidate[path]
        for name, data in live.items():
            assert (result / name.removeprefix("docs/site/")).read_bytes() == data
    candidate.clear()
    result = overlay.stage_site(destination=tmp_path / "normal-push")
    assert (result / "index.html").read_text() == "reviewed main site"
    for name, data in live.items():
        assert (result / name.removeprefix("docs/site/")).read_bytes() == data
    receipt = json.loads((result / "packs/tester/publication.json").read_bytes())
    assert receipt["assets"] == {
        name: hashlib.sha256(data).hexdigest() for name, data in live.items()
    }
    updated = envelope("benchmark-catalog-1:" + "c" * 64, 3, "legacy-input")
    monkeypatch.setattr(
        overlay.legacy, "overlay", lambda commit: (RECORDING, updated, 3)
    )
    result = overlay.stage_site(
        recording_commit="b" * 40, destination=tmp_path / "legacy-input"
    )
    assert (result / RECORDING.removeprefix("docs/site/")).read_bytes() == updated
    for name in (PROFILE, SHADER):
        assert (result / name.removeprefix("docs/site/")).read_bytes() == live[name]
    candidate[SHADER] = envelope("shader-lists-1", 1, "rollback")
    with pytest.raises(ValueError, match="sequence"):
        overlay.stage_site(asset_commit="b" * 40, destination=tmp_path / "bad")
    assert not (tmp_path / "bad").exists()


def test_live_receipt_hash_missing_asset_and_invalid_signature_fail(monkeypatch):
    raw = envelope("benchmark-catalog-1:" + "a" * 64, 1, "r1")
    receipt = json.dumps(
        {"schema": 1, "assets": {RECORDING: hashlib.sha256(raw).hexdigest()}}
    ).encode()
    responses = {
        overlay.RECEIPT: receipt,
        overlay.MARKER: overlay.MARKER_DATA,
        RECORDING: raw,
    }

    def fetch(path, maximum, **kwargs):
        if path not in responses:
            raise ValueError("missing asset")
        return responses[path]

    monkeypatch.setattr(overlay, "fetch", fetch)
    monkeypatch.setattr(overlay, "verify_asset", lambda path, data: json.loads(data))
    assert overlay.live_assets() == {RECORDING: raw}
    responses[RECORDING] = raw + b" "
    with pytest.raises(ValueError, match="hash"):
        overlay.live_assets()
    responses.pop(RECORDING)
    with pytest.raises(ValueError, match="missing"):
        overlay.live_assets()
    responses[RECORDING] = raw

    def invalid(path, data):
        raise ValueError("invalid signature")

    monkeypatch.setattr(overlay, "verify_asset", invalid)
    with pytest.raises(ValueError, match="signature"):
        overlay.live_assets()


def test_receipt_transition_preserves_live_recording_and_later_missing_receipt_fails(
    monkeypatch,
):
    raw = envelope("benchmark-catalog-1:" + "a" * 64, 1, "r1")
    responses = {overlay.RECEIPT: None, overlay.MARKER: None, RECORDING: raw}
    monkeypatch.setattr(
        overlay, "fetch", lambda path, maximum, **kwargs: responses[path]
    )
    monkeypatch.setattr(overlay, "verify_asset", lambda path, data: json.loads(data))
    assert overlay.live_assets() == {RECORDING: raw}
    responses[overlay.MARKER] = overlay.MARKER_DATA
    with pytest.raises(ValueError, match="receipt is missing"):
        overlay.live_assets()
    responses[overlay.RECEIPT] = b"not json"
    with pytest.raises(ValueError, match="invalid JSON"):
        overlay.live_assets()


def test_real_signed_shaders_and_profile_shards_bind_the_path(tmp_path, monkeypatch):
    from cryptography.exceptions import InvalidSignature
    from tools import release_validation as validation
    from tools.test_release_assets import signed_envelope

    text = (
        "vs 0000000000000001 0 0\nps 2fba1332b14beb55 "
        + "00" * 252
        + "\n"
        + "pipeline 0000000000000001.0.0 2fba1332b14beb55 0 0 0 0 15 15 1 "
        + "00" * 16
        + " 0 0\n"
    )
    shader, _ = signed_envelope(
        monkeypatch, "shaders", "shader-lists-1", {"a10.txt": text.encode()}
    )
    assert overlay.verify_asset(SHADER, shader) == {
        "compatibility": "shader-lists-1",
        "sequence": 1,
    }
    bad = json.loads(shader)
    bad["signature"] = "A" * 86 + "=="
    with pytest.raises((InvalidSignature, ValueError)):
        overlay.verify_asset(SHADER, json.dumps(bad).encode())
    files = validation.packs.data_pack.device_profile_snapshot(
        validation.packs.data_pack.ROOT / "profiles", "ios", "iPhone18,1"
    )
    profile, _ = signed_envelope(
        monkeypatch, "profiles", "device-profiles-1:ios/iPhone18,1/_", files
    )
    assert overlay.verify_asset(PROFILE, profile)["sequence"] == 1
    with pytest.raises(ValueError, match="compatibility"):
        overlay.verify_asset(PROFILE.replace("iPhone18,1", "iPhone18,2"), profile)
    with pytest.raises(ValueError, match="compatibility"):
        overlay.verify_asset(SHADER, profile)


def test_real_signed_recording_catalog_binds_filename_guest(tmp_path, monkeypatch):
    from tools import data_pack
    from tools.test_benchmark_catalog import _recording, _write_catalog, _cases
    from tools.test_release_assets import signed_envelope

    _recording(tmp_path)
    _write_catalog(tmp_path, _cases())
    domain = "benchmark-catalog-1:" + "a" * 64
    files = data_pack.snapshot(tmp_path, "recordings", domain)
    raw, _ = signed_envelope(monkeypatch, "recordings", domain, files)
    assert overlay.verify_asset(RECORDING, raw)["compatibility"] == domain
    with pytest.raises(ValueError, match="filename"):
        overlay.verify_asset(
            "docs/site/packs/tester/recordings-" + "b" * 64 + ".json", raw
        )


def test_candidate_git_boundary_rejects_parent_site_paths_and_symlinks(monkeypatch):
    source, commit = "a" * 40, "b" * 40
    for key, value in {
        "GITHUB_ACTIONS": "true",
        "GITHUB_REF": "refs/heads/main",
        "GITHUB_EVENT_NAME": "workflow_dispatch",
        "GITHUB_SHA": source,
        "GITHUB_REPOSITORY": "example/visr",
    }.items():
        monkeypatch.setenv(key, value)
    state = {"path": SHADER, "parent": source, "mode": "100644"}
    raw = envelope("shader-lists-1", 2, "s2")
    calls = []

    def git(*args):
        calls.append(args)
        if args == ("rev-parse", "HEAD"):
            return source.encode()
        if args[0] in {"status", "fetch"}:
            return b""
        if args[0] == "rev-list":
            return (commit + " " + state["parent"]).encode()
        if args[0] == "diff":
            return state["path"].encode() + b"\0"
        if args[0] == "ls-tree":
            return (state["mode"] + " blob " + "c" * 40 + " " + state["path"]).encode()
        if args[:2] == ("cat-file", "-s"):
            return str(len(raw)).encode()
        if args[:2] == ("cat-file", "blob"):
            return raw
        raise AssertionError(args)

    monkeypatch.setattr(overlay.legacy, "git", git)
    monkeypatch.setattr(overlay, "verify_asset", lambda path, data: json.loads(data))
    assert overlay.candidate_assets(commit) == {SHADER: raw}
    assert any(
        x[:5]
        == (
            "fetch",
            "--no-tags",
            "--depth=2",
            "--no-write-fetch-head",
            "https://github.com/example/visr.git",
        )
        for x in calls
    )
    state["parent"] = "d" * 40
    with pytest.raises(ValueError, match="parent"):
        overlay.candidate_assets(commit)
    state["parent"] = source
    state["path"] = "docs/site/index.html"
    with pytest.raises(ValueError, match="asset paths"):
        overlay.candidate_assets(commit)
    state["path"] = SHADER
    state["mode"] = "120000"
    with pytest.raises(ValueError, match="regular"):
        overlay.candidate_assets(commit)


def test_https_fetch_rejects_redirect_and_oversize(monkeypatch):
    import urllib.error

    class Opener:
        def open(self, url, timeout):
            assert url.startswith("https://") and timeout == 45
            raise urllib.error.HTTPError(url, 302, "redirect", Message(), None)

    monkeypatch.setattr(overlay.urllib.request, "build_opener", lambda guard: Opener())
    with pytest.raises(ValueError, match="unavailable"):
        overlay.fetch(RECORDING, 8)

    class Response:
        def __enter__(self):
            return self

        def __exit__(self, *args):
            return None

        def read(self, maximum):
            assert maximum == 9
            return b"x" * 9

    monkeypatch.setattr(Opener, "open", lambda *args, **kwargs: Response())
    with pytest.raises(ValueError, match="bounded"):
        overlay.fetch(RECORDING, 8)


def test_first_profile_only_update_emits_valid_metadata_and_survives_redeploy(
    tmp_path, monkeypatch
):
    real_live_assets = overlay.live_assets
    (tmp_path / "docs/site").mkdir(parents=True)
    (tmp_path / "docs/site/index.html").write_text("main site")
    monkeypatch.setattr(overlay, "ROOT", tmp_path)
    monkeypatch.setattr(overlay, "trusted_source", lambda: "a" * 40)
    monkeypatch.setattr(overlay, "source_assets", lambda: {})
    monkeypatch.setattr(overlay, "verify_asset", lambda path, data: json.loads(data))
    recording = envelope("benchmark-catalog-1:" + "a" * 64, 1, "recording")
    profile = envelope("device-profiles-1:ios/iPhone18,1/_", 1, "profile")
    monkeypatch.setattr(overlay, "live_assets", lambda: {RECORDING: recording})
    monkeypatch.setattr(overlay, "candidate_assets", lambda commit: {PROFILE: profile})
    first = overlay.stage_site(asset_commit="b" * 40, destination=tmp_path / "first")
    receipt = (first / "packs/tester/publication.json").read_bytes()
    assert overlay.parse_receipt(receipt) == {
        RECORDING: hashlib.sha256(recording).hexdigest(),
        PROFILE: hashlib.sha256(profile).hexdigest(),
    }
    assert (
        first / "packs/tester/publication-format.txt"
    ).read_bytes() == overlay.MARKER_DATA
    responses = {
        overlay.RECEIPT: receipt,
        overlay.MARKER: overlay.MARKER_DATA,
        RECORDING: recording,
        PROFILE: profile,
    }

    def fetch(path, maximum, **kwargs):
        return responses[path]

    monkeypatch.setattr(overlay, "fetch", fetch)
    # Exercise actual receipt/marker/hash validation on the next source deployment.
    monkeypatch.setattr(overlay, "live_assets", real_live_assets)
    second = overlay.stage_site(destination=tmp_path / "normal")
    assert (second / PROFILE.removeprefix("docs/site/")).read_bytes() == profile
    assert (second / RECORDING.removeprefix("docs/site/")).read_bytes() == recording
    responses[overlay.MARKER] = None
    with pytest.raises(ValueError, match="marker is missing"):
        overlay.live_assets()
    responses[overlay.MARKER] = b"wrong"
    with pytest.raises(ValueError, match="marker is missing or invalid"):
        overlay.live_assets()


def test_generation_two_shards_bind_their_directory_and_keep_their_revision(monkeypatch):
    from tools import release_validation as validation
    from tools.test_release_assets import signed_envelope

    data_pack = validation.packs.data_pack
    files = data_pack.device_profile_snapshot(data_pack.ROOT / "profiles", "ios", "iPhone18,1", generation=2)
    profile, _ = signed_envelope(monkeypatch, "profiles", "device-profiles-2:ios/iPhone18,1/_", files)
    revision = overlay.profile_revision.check()
    assert overlay.verify_asset(PROFILE_2, profile) == {
        "compatibility": "device-profiles-2:ios/iPhone18,1/_", "sequence": 1, "revision": revision}
    # each directory admits only its own generation
    with pytest.raises(ValueError, match="compatibility"):
        overlay.verify_asset(PROFILE, profile)
    old_files = data_pack.device_profile_snapshot(data_pack.ROOT / "profiles", "ios", "iPhone18,1")
    old, _ = signed_envelope(monkeypatch, "profiles", "device-profiles-1:ios/iPhone18,1/_", old_files)
    with pytest.raises(ValueError, match="compatibility"):
        overlay.verify_asset(PROFILE_2, old)


def test_a_lower_profile_revision_cannot_replace_a_published_shard(monkeypatch):
    shards = {
        b"new": {"compatibility": "device-profiles-2:ios/iPhone18,1/_", "sequence": 2, "revision": 5},
        b"rollback": {"compatibility": "device-profiles-2:ios/iPhone18,1/_", "sequence": 3, "revision": 4},
        b"next": {"compatibility": "device-profiles-2:ios/iPhone18,1/_", "sequence": 3, "revision": 6},
    }
    monkeypatch.setattr(overlay, "verify_asset", lambda path, data: shards[data])
    with pytest.raises(ValueError, match="revision"):
        overlay.advance(PROFILE_2, b"new", b"rollback")
    overlay.advance(PROFILE_2, b"new", b"next")


def test_a_retired_recording_feed_leaves_the_site_but_other_live_feeds_stay(tmp_path, monkeypatch):
    (tmp_path / "docs/site").mkdir(parents=True)
    retired = sorted(overlay.RETIRED_FEEDS)[0]
    other = "docs/site/packs/tester/recordings-" + "9" * 64 + ".json"
    live = {
        RECORDING: envelope("benchmark-catalog-1:" + "a" * 64, 1, "r"),
        retired: envelope("benchmark-catalog-1:" + "b" * 64, 1, "old"),
        other: envelope("benchmark-catalog-1:" + "9" * 64, 1, "kept"),
    }
    monkeypatch.setattr(overlay, "ROOT", tmp_path)
    monkeypatch.setattr(overlay, "trusted_source", lambda: "a" * 40)
    monkeypatch.setattr(overlay, "verify_asset", lambda path, data: json.loads(data))
    monkeypatch.setattr(overlay, "source_assets", lambda: {RECORDING: live[RECORDING]})
    monkeypatch.setattr(overlay, "live_assets", lambda: dict(live))
    result = overlay.stage_site(destination=tmp_path / "stage")
    assert not (result / retired.removeprefix("docs/site/")).exists()
    assert (result / other.removeprefix("docs/site/")).read_bytes() == live[other]
    receipt = json.loads((result / "packs/tester/publication.json").read_bytes())
    assert retired not in receipt["assets"] and other in receipt["assets"]
