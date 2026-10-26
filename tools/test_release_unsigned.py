"""The unsigned apps attached to a release: unsigned, from the tag's guest, and attached without touching the body."""

from __future__ import annotations

import hashlib
import plistlib
import zipfile
from pathlib import Path
from typing import Any

import pytest

from tools import release, release_unsigned
from tools.test_release import policy

GUEST = "a" * 64
TEAM = "TEAMID1234"


def manifest() -> dict[str, Any]:
    return {
        "tag": "visr-v0.5.1",
        "version": "0.5.1",
        "build_number": "20261026.31927",
        "source_date_epoch": "1792984767",
        "build_policy": {**policy(), "guest_sha256": GUEST},
    }


def write_ipa(folder: Path, platform: str, *, extra: dict[str, bytes] | None = None,
              executable: bytes | None = None, version: str = "0.5.1") -> Path:
    info = {
        "CFBundleShortVersionString": version,
        "CFBundleVersion": "20261026.31927",
        "DTPlatformName": release.PLATFORMS[platform][1],
        "CFBundleExecutable": "VISR",
    }
    files = {
        "Info.plist": plistlib.dumps(info),
        "VISR": executable if executable is not None else b"\xcf\xfa\xed\xfe" + b"\0" * 28 + GUEST.encode(),
        "Settings.bundle/Root.plist": plistlib.dumps({}),
        **(extra or {}),
    }
    ipa = folder / release_unsigned.IPA_NAMES[platform]
    with zipfile.ZipFile(ipa, "w") as archive:
        for name, data in files.items():
            archive.writestr(f"Payload/VISR.app/{name}", data)
    digest = hashlib.sha256(ipa.read_bytes()).hexdigest()
    ipa.with_name(ipa.name + ".sha256").write_text(f"{digest}  {ipa.name}\n")
    return ipa


@pytest.fixture
def unsigned(monkeypatch: pytest.MonkeyPatch) -> list[Path]:
    checked: list[Path] = []

    def state(path: Path) -> str:
        checked.append(path)
        return f"{path}: code object is not signed at all"

    monkeypatch.setattr(release_unsigned, "signature_state", state)
    return checked


def test_build_command_is_unsigned_and_leaves_out_icloud_features() -> None:
    for platform in release_unsigned.PLATFORMS:
        command = [str(item) for item in release_unsigned.build_command(platform, manifest(), Path("/out"))]
        assert "--unsigned" in command
        for flag in ("--team", "--signing-profile", "--app-store-package", "--cloud-saves", "--shader-upload",
                     "--perf-reports", "--benchmark-upload", "--upload"):
            assert flag not in command
        assert command[command.index("--marketing-version") + 1] == "0.5.1"
        assert command[command.index("--build-number") + 1] == "20261026.31927"
        assert command[command.index("--ipa") + 1] == f"/out/{release_unsigned.IPA_NAMES[platform]}"
        assert (f"--{platform}" in command) == (platform != "ios")


def test_verify_accepts_an_unsigned_app_with_the_release_guest(tmp_path: Path, unsigned: list[Path]) -> None:
    for platform in release_unsigned.PLATFORMS:
        write_ipa(tmp_path, platform)
    assets = release_unsigned.verify_directory(tmp_path, manifest(), team_id=TEAM, require_guest=False)
    assert [item["name"] for item in assets] == [release_unsigned.IPA_NAMES[p] for p in release_unsigned.PLATFORMS]
    assert {item["guest_sha256"] for item in assets} == {GUEST}
    # the app bundle and its executable, never the resource-only Settings.bundle
    assert {path.name for path in unsigned} == {"VISR.app", "VISR"}


@pytest.mark.parametrize(
    "extra, message",
    [
        ({"embedded.mobileprovision": b"profile"}, "signing material"),
        ({"_CodeSignature/CodeResources": b"x"}, "signing material"),
        ({"PlugIns/A.appex/_CodeSignature/x": b"x"}, "_CodeSignature"),
        ({"Licenses/team.txt": TEAM.encode()}, "team ID"),
        ({"Licenses/team16.txt": TEAM.encode("utf-16-le")}, "team ID"),
    ],
)
def test_verify_rejects_signing_material_and_the_team_id(
    tmp_path: Path, unsigned: list[Path], extra: dict[str, bytes], message: str
) -> None:
    ipa = write_ipa(tmp_path, "ios", extra=extra)
    with pytest.raises(ValueError, match=message):
        release_unsigned.verify_ipa(ipa, manifest(), "ios", team_id=TEAM)


def test_verify_rejects_a_signed_app(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(release_unsigned, "signature_state", lambda path: f"Executable={path}\nTeamIdentifier=not set")
    ipa = write_ipa(tmp_path, "tvos")
    with pytest.raises(ValueError, match="as unsigned"):
        release_unsigned.verify_ipa(ipa, manifest(), "tvos")


def test_verify_rejects_another_guest_version_or_checksum(tmp_path: Path, unsigned: list[Path]) -> None:
    ipa = write_ipa(tmp_path, "ios", executable=b"\xcf\xfa\xed\xfe" + b"b" * 64)
    with pytest.raises(ValueError, match="release guest"):
        release_unsigned.verify_ipa(ipa, manifest(), "ios")
    ipa = write_ipa(tmp_path, "ios", version="0.1.2")
    with pytest.raises(ValueError, match="version"):
        release_unsigned.verify_ipa(ipa, manifest(), "ios")
    ipa = write_ipa(tmp_path, "ios")
    with pytest.raises(ValueError, match="built guest differs"):
        release_unsigned.verify_ipa(ipa, manifest(), "ios", guest=b"another guest")
    ipa.with_name(ipa.name + ".sha256").write_text("0" * 64 + f"  {ipa.name}\n")
    with pytest.raises(ValueError, match="does not match"):
        release_unsigned.verify_ipa(ipa, manifest(), "ios")


def test_verify_rejects_entries_outside_the_app(tmp_path: Path, unsigned: list[Path]) -> None:
    ipa = write_ipa(tmp_path, "ios")
    with zipfile.ZipFile(ipa, "a") as archive:
        archive.writestr("Payload/Other.app/x", b"x")
    digest = hashlib.sha256(ipa.read_bytes()).hexdigest()
    ipa.with_name(ipa.name + ".sha256").write_text(f"{digest}  {ipa.name}\n")
    with pytest.raises(ValueError, match="unexpected archive entry"):
        release_unsigned.verify_ipa(ipa, manifest(), "ios")


def test_verify_directory_requires_the_guest_and_only_release_files(tmp_path: Path, unsigned: list[Path]) -> None:
    for platform in release_unsigned.PLATFORMS:
        write_ipa(tmp_path, platform)
    with pytest.raises(ValueError, match="halo_guest.elf is missing"):
        release_unsigned.verify_directory(tmp_path, manifest())
    (tmp_path / "VISR-signed.ipa").write_bytes(b"x")
    with pytest.raises(ValueError, match="Unexpected files"):
        release_unsigned.verify_directory(tmp_path, manifest(), require_guest=False)


class FakeGitHub:
    def __init__(self, assets: list[dict[str, Any]]) -> None:
        self.assets = assets
        self.calls: list[tuple[str, str]] = []
        self.uploads: dict[str, bytes] = {}

    def request(self, method: str, path: str, body: Any = None) -> Any:
        self.calls.append((method, path))
        if method == "DELETE":
            number = int(path.rsplit("/", 1)[1])
            self.assets = [item for item in self.assets if item["id"] != number]
            return {}
        assert method == "GET" and body is None
        return {"id": 7, "body": "notes + journal", "upload_url": "https://uploads.example/r/7/assets{?name,label}",
                "assets": list(self.assets)}

    def upload(self, url: str, data: bytes) -> Any:
        name = url.split("?name=", 1)[1]
        self.calls.append(("UPLOAD", name))
        self.uploads[name] = data
        self.assets.append({"id": 100 + len(self.assets), "name": name, "size": len(data), "state": "uploaded",
                            "digest": "sha256:" + hashlib.sha256(data).hexdigest()})
        return {}


def local_assets(folder: Path) -> list[dict[str, Any]]:
    items = []
    for name in release_unsigned.asset_names():
        data = f"contents of {name}".encode()
        (folder / name).write_bytes(data)
        items.append({"name": name, "path": folder / name, "size": len(data),
                      "sha256": hashlib.sha256(data).hexdigest()})
    return items


def test_attach_keeps_matching_assets_and_replaces_partial_ones(tmp_path: Path) -> None:
    assets = local_assets(tmp_path)
    good, partial = assets[0], assets[2]
    github = FakeGitHub([
        {"id": 1, "name": good["name"], "size": good["size"], "state": "uploaded",
         "digest": "sha256:" + good["sha256"]},
        {"id": 2, "name": partial["name"], "size": 3, "state": "starter", "digest": None},
    ])
    release.attach_unsigned_assets(github, "/repos/o/r/releases", {"id": 7}, assets)
    assert ("DELETE", "/repos/o/r/releases/assets/2") in github.calls
    assert good["name"] not in github.uploads
    assert set(github.uploads) == {item["name"] for item in assets} - {good["name"]}
    assert not any(method in ("PATCH", "POST") for method, _ in github.calls)


def test_attach_fails_when_the_uploaded_digest_differs(tmp_path: Path) -> None:
    assets = local_assets(tmp_path)
    github = FakeGitHub([])
    original = github.upload

    def corrupt(url: str, data: bytes) -> Any:
        return original(url, data + b"!")

    github.upload = corrupt  # type: ignore[method-assign]
    with pytest.raises(ValueError, match="differ from the verified unsigned builds"):
        release.attach_unsigned_assets(github, "/repos/o/r/releases", {"id": 7}, assets)


def test_publish_refuses_a_release_with_other_assets(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    notes = "## 0.5.1\n\n- Fix input"
    manifest_value = {"tag": "visr-v0.5.1", "version": "0.5.1",
                      "notes_sha256": hashlib.sha256(notes.encode()).hexdigest()}
    (tmp_path / "CHANGELOG.md").write_text(notes)

    class GitHub:
        def request(self, method: str, path: str, body: Any = None) -> Any:
            return [{"tag_name": "visr-v0.5.1", "draft": True, "body": notes, "assets": [{"name": "VISR.ipa"}]}]

    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "publish_context", lambda _: (tmp_path, manifest_value))
    monkeypatch.setattr(release, "API", lambda base, token: GitHub())
    monkeypatch.setenv("GH_TOKEN", "dummy")
    monkeypatch.setenv("GITHUB_REPOSITORY", "example/app")
    with pytest.raises(ValueError, match="only the unsigned builds"):
        release.publish(type("Args", (), {"source_root": tmp_path, "unsigned_assets": tmp_path})())
