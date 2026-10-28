"""Checked-in Pages assets remain the only deployment input."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys

import pytest

from tools import pages_asset_overlay as overlay

PROFILE = "docs/site/packs/tester/profiles/ios/iPhone18,1/_.json"
SHADER = "docs/site/packs/tester/shaders.json"
RECORDING = "docs/site/packs/tester/recordings.json"
RECORDING_OLD = (
    "docs/site/packs/tester/recordings-"
    "4d6924ec86afba868b1fd994bcc323d441b18991ef5367cebca18a6b5cb97d2d.json"
)
RECORDING_SECOND = (
    "docs/site/packs/tester/recordings-"
    "6c831c00ef902ff90589f77cd834291dfde17dcec189cd3bdff22fd168f3334e.json"
)
RECORDING_050 = (
    "docs/site/packs/tester/recordings-"
    "28e06eaaa33443bbcce7ad8b4415d73bf3d21ab2d00a5846d8b85ec3b183f995.json"
)
RECORDING_051 = (
    "docs/site/packs/tester/recordings-"
    "d57c1206e07347c174974dac9e6bdfd2df6bea9194a991198e12048b4f239b92.json"
)
RECORDING_052 = (
    "docs/site/packs/tester/recordings-"
    "a949cfcc94abf93df5bd091bd9d27a6933dbc688b81f52c3ea863fa720ff99eb.json"
)
RECORDING_060 = (
    "docs/site/packs/tester/recordings-"
    "28b14010a886a159afff1bec13ec7f516b5235f270934906f15ff59ff10b0fd1.json"
)
RECORDING_061 = (
    "docs/site/packs/tester/recordings-"
    "c7fe63a3433bb5ef215fbd498679caf5688f56663b65879d496d39981f482b91.json"
)
RECORDING_062 = (
    "docs/site/packs/tester/recordings-"
    "36970174a94fd63fb36d43dadf6011911db23fb49f68291beca46b4d71963c0f.json"
)
RECORDING_063 = (
    "docs/site/packs/tester/recordings-"
    "c2ce7f3b23742865e51ad2716fd5dd15ecfe14c68c205cea5ba5be33e3d00b89.json"
)
RECEIPT = "docs/site/packs/tester/publication.json"
MARKER = "docs/site/packs/tester/publication-format.txt"


def make_envelope(compatibility: str, sequence: int) -> bytes:
    return json.dumps(
        {"compatibility": compatibility, "sequence": sequence}, sort_keys=True
    ).encode()


def fixture_site(root: Path) -> dict[str, bytes]:
    assets = {
        PROFILE: make_envelope("device-profiles-1:ios/iPhone18,1/_", 1),
        SHADER: make_envelope("shader-lists-1", 2),
        RECORDING: make_envelope("benchmark-catalog-1:" + "c" * 64, 4),
        RECORDING_OLD: make_envelope("benchmark-replay-1:" + "d" * 64, 1),
        RECORDING_SECOND: make_envelope("benchmark-catalog-1:" + "e" * 64, 2),
    }
    for name, data in assets.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    receipt = {
        "schema": 1,
        "assets": {name: hashlib.sha256(data).hexdigest() for name, data in assets.items()},
    }
    (root / RECEIPT).write_text(json.dumps(receipt, sort_keys=True, separators=(",", ":")) + "\n")
    (root / MARKER).write_bytes(overlay.MARKER_DATA)
    legacy = root / "docs/site/packs/tester/profiles-2.json"
    legacy.write_bytes(b"preserved legacy static file")
    (root / "docs/site/index.html").parent.mkdir(parents=True, exist_ok=True)
    (root / "docs/site/index.html").write_bytes(b"main site")
    return assets


def configure_main_site(monkeypatch: pytest.MonkeyPatch, root: Path) -> dict[str, bytes]:
    assets = fixture_site(root)
    monkeypatch.setattr(overlay, "ROOT", root)
    monkeypatch.setattr(overlay, "trusted_source", lambda: "f" * 40)

    def verify(path: str, data: bytes) -> dict[str, object]:
        value = json.loads(data)
        if value.get("invalid_signature"):
            raise ValueError("Invalid signed asset")
        if path == PROFILE:
            assert value["compatibility"] == "device-profiles-1:ios/iPhone18,1/_"
        return {"compatibility": value["compatibility"], "sequence": value["sequence"]}

    monkeypatch.setattr(overlay, "verify_asset", verify)
    monkeypatch.setattr(overlay, "fetch", lambda *_args, **_kwargs: pytest.fail("network fetch used"))
    monkeypatch.setattr(overlay, "live_assets", lambda: pytest.fail("live overlay used"))
    monkeypatch.setattr(overlay, "candidate_assets", lambda *_args: pytest.fail("commit overlay used"))
    return assets


def test_checked_in_staging_copies_assets_and_site_without_network(tmp_path, monkeypatch):
    assets = configure_main_site(monkeypatch, tmp_path)
    destination = tmp_path / "build/pages"

    result = overlay.stage_main_site(destination=destination)

    assert result == destination
    assert (destination / "index.html").read_bytes() == b"main site"
    assert (destination / "packs/tester/profiles-2.json").read_bytes() == b"preserved legacy static file"
    assert (destination / "packs/tester/publication-format.txt").read_bytes() == overlay.MARKER_DATA
    for path, data in assets.items():
        assert (destination / Path(path).relative_to("docs/site")).read_bytes() == data
    receipt = (destination / "packs/tester/publication.json").read_bytes()
    assert overlay.parse_receipt(receipt) == {
        path: hashlib.sha256(data).hexdigest() for path, data in assets.items()
    }


def test_checked_in_staging_rejects_missing_referenced_asset(tmp_path, monkeypatch):
    configure_main_site(monkeypatch, tmp_path)
    (tmp_path / SHADER).unlink()
    with pytest.raises(ValueError, match="receipt|missing|asset"):
        overlay.stage_main_site(destination=tmp_path / "build/pages")


def test_checked_in_staging_rejects_hash_mismatch(tmp_path, monkeypatch):
    configure_main_site(monkeypatch, tmp_path)
    (tmp_path / SHADER).write_bytes(b"changed")
    with pytest.raises(ValueError, match="hash"):
        overlay.stage_main_site(destination=tmp_path / "build/pages")


def test_checked_in_staging_rejects_invalid_signature(tmp_path, monkeypatch):
    configure_main_site(monkeypatch, tmp_path)
    (tmp_path / SHADER).write_bytes(
        make_envelope("shader-lists-1", 2)[:-1] + b",\"invalid_signature\":true}"
    )
    receipt_path = tmp_path / RECEIPT
    receipt = json.loads(receipt_path.read_bytes())
    receipt["assets"][SHADER] = hashlib.sha256((tmp_path / SHADER).read_bytes()).hexdigest()
    receipt_path.write_text(json.dumps(receipt, sort_keys=True, separators=(",", ":")) + "\n")
    with pytest.raises(ValueError, match="Invalid signed asset"):
        overlay.stage_main_site(destination=tmp_path / "build/pages")


def test_checked_in_staging_rejects_invalid_sequence(tmp_path, monkeypatch):
    configure_main_site(monkeypatch, tmp_path)
    (tmp_path / SHADER).write_bytes(make_envelope("shader-lists-1", 0))
    receipt_path = tmp_path / RECEIPT
    receipt = json.loads(receipt_path.read_bytes())
    receipt["assets"][SHADER] = hashlib.sha256((tmp_path / SHADER).read_bytes()).hexdigest()
    receipt_path.write_text(json.dumps(receipt, sort_keys=True, separators=(",", ":")) + "\n")
    with pytest.raises(ValueError, match="sequence"):
        overlay.stage_main_site(destination=tmp_path / "build/pages")


def test_checked_in_staging_rejects_unrecognized_feed_path(tmp_path, monkeypatch):
    configure_main_site(monkeypatch, tmp_path)
    rogue = tmp_path / "docs/site/packs/tester/not-a-feed.json"
    rogue.write_bytes(b"unexpected")
    with pytest.raises(ValueError, match="not an allowed signed asset path"):
        overlay.stage_main_site(destination=tmp_path / "build/pages")


def test_checked_in_staging_enforces_total_size_cap(tmp_path, monkeypatch):
    configure_main_site(monkeypatch, tmp_path)
    monkeypatch.setattr(overlay, "MAX_TOTAL", 16)
    with pytest.raises(ValueError, match="bounded total size"):
        overlay.stage_main_site(destination=tmp_path / "build/pages")


def test_checked_in_staging_enforces_per_asset_size_cap(tmp_path, monkeypatch):
    configure_main_site(monkeypatch, tmp_path)
    original_limit = overlay.limit_for
    monkeypatch.setattr(
        overlay,
        "limit_for",
        lambda path: 3 if path == SHADER else original_limit(path),
    )
    with pytest.raises(ValueError, match="bounded regular file"):
        overlay.stage_main_site(destination=tmp_path / "build/pages")


@pytest.mark.parametrize("argument", ["--asset-commit", "--recording-commit", "--unknown"])
def test_cli_rejects_old_commit_inputs_and_unknown_arguments(monkeypatch, argument):
    monkeypatch.setattr(sys, "argv", ["pages_asset_overlay.py", argument, "a" * 40])
    with pytest.raises(SystemExit) as error:
        overlay.main()
    assert error.value.code == 2


def test_workflow_has_no_alternate_asset_inputs_or_live_fetch():
    root = Path(__file__).resolve().parents[1]
    workflow = (root / ".github/workflows/pages.yml").read_text()
    assert "recording_commit:" not in workflow
    assert "asset_commit:" not in workflow
    assert "inputs." not in workflow
    assert "workflow_dispatch:" in workflow
    assert "python tools/pages_asset_overlay.py" in workflow
    assert "--asset-commit" not in workflow
    assert "--recording-commit" not in workflow
    assert "RECORDING_COMMIT" not in workflow
    assert "ASSET_COMMIT" not in workflow


def test_current_release_assets_are_checked_in_and_exact():
    root = Path(__file__).resolve().parents[1]
    assets = overlay.checked_in_assets()
    profiles = [path for path in assets if "/profiles/" in path]
    recordings = [path for path in assets if "recordings" in Path(path).name]
    assert len(profiles) == 20
    assert len([path for path in assets if "/device-profiles-2/" in path]) == 55
    assert len(recordings) == 10
    assert set(assets) == set(overlay.parse_receipt((root / RECEIPT).read_bytes()))
    assert hashlib.sha256(assets[RECORDING]).hexdigest() == (
        "8977e0e49588399d1c0f867d98f955b8a815b0c05e7463df2d1e92ae3ba64d89"
    )
    assert hashlib.sha256(assets[RECORDING_OLD]).hexdigest() == (
        "69655d46646b69f259e66803204e840306a32740fffe904ce9517550c4650956"
    )
    assert hashlib.sha256(assets[RECORDING_SECOND]).hexdigest() == (
        "262e9073873bed77aab3705d4490c5272eb20aa3c2ad0d66c4665be2b1471df8"
    )
    assert hashlib.sha256(assets[RECORDING_050]).hexdigest() == (
        "da937b8f9169e74d14aeac05d187bc34c50fd8e111be4ea8cb9713e2ca22824e"
    )
    assert hashlib.sha256(assets[RECORDING_051]).hexdigest() == (
        "b1dd79313a5e60043e8f9e91b10152950c44d0897fab5f842155c069e17d5aa9"
    )
    assert hashlib.sha256(assets[RECORDING_052]).hexdigest() == (
        "90bd98c96033913ef68771111369efcc44d2aff0087579b72ff4fab02ce12352"
    )
    assert hashlib.sha256(assets[RECORDING_060]).hexdigest() == (
        "4ef803888532401d66d132d2963ac9ff8ef9cef017fd0769f576452b38b7f5b4"
    )
    assert hashlib.sha256(assets[RECORDING_061]).hexdigest() == (
        "b0f48ea1d1eeb77f8872d9fd0149e2e40e1d93913727a3dce4e000aa8b4d363d"
    )
    assert hashlib.sha256(assets[RECORDING_062]).hexdigest() == (
        "be6dddab12c382837eb40090e1cbaa3d614736cffef5de68ccf0981b526e516f"
    )
    assert hashlib.sha256(assets[RECORDING_063]).hexdigest() == (
        "ca0a7e1e956368d3a963e5b392416f3269e35607a9523b15c3ee8ba892de9990"
    )
    envelope = json.loads(assets[SHADER])
    manifest = overlay.validation.packs.decode_json(
        overlay.validation.packs.decode_base64(envelope["manifest"], 65536)
    )
    payload = overlay.validation.packs.decode_json(
        overlay.validation.packs.decode_base64(envelope["payload"], 16 * 1024 * 1024)
    )
    assert manifest["sequence"] == 9
    assert manifest["compatibility"] == "shader-lists-2"
    assert len(payload["files"]) == 24
    assert "b40.txt" in payload["files"]


def test_actual_main_site_stages_exact_checked_in_assets_without_network(tmp_path, monkeypatch):
    monkeypatch.setattr(overlay, "trusted_source", lambda: "f" * 40)
    monkeypatch.setattr(overlay, "fetch", lambda *_args, **_kwargs: pytest.fail("network fetch used"))
    monkeypatch.setattr(overlay, "live_assets", lambda: pytest.fail("live overlay used"))
    monkeypatch.setattr(overlay, "candidate_assets", lambda *_args: pytest.fail("commit overlay used"))
    destination = tmp_path / "build/pages"

    overlay.stage_main_site(destination=destination)

    for name, expected in overlay.checked_in_assets().items():
        staged = destination / Path(name).relative_to("docs/site")
        assert hashlib.sha256(staged.read_bytes()).hexdigest() == hashlib.sha256(expected).hexdigest()
    assert (destination / "index.html").read_bytes() == (Path(__file__).resolve().parents[1] / "docs/site/index.html").read_bytes()
