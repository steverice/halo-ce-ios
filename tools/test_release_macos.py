"""The native Mac app in the release path: archive, guest check and App Store export, never upload by itself."""

from __future__ import annotations

import hashlib
import json
import plistlib
from pathlib import Path
from typing import Any

import pytest

from tools import release
from tools import release_signing
from tools import release_validation as validation
from tools.test_release_assets import asset_policy


def manifest(policy: dict[str, Any]) -> dict[str, Any]:
    return {"version": "0.5.0", "build_number": "20261023.1", "build_policy": policy}


def test_platforms_accept_macos_with_the_others() -> None:
    assert release.platforms("ios,macos") == ["ios", "macos"]
    assert release.PLATFORMS["macos"] == ("MAC_OS", "macosx")


def test_mac_release_build_is_the_native_app_on_the_tester_feeds(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("APPLE_TEAM_ID", "TEAM")
    monkeypatch.setenv("APPLE_BUNDLE_ID", "org.steverice.visr")
    policy = {**asset_policy(), "benchmark_upload": "on"}
    command = [str(part) for part in release.platform_build_command(
        "macos", manifest(policy), policy, "MAC-UUID", Path("build/release/macos"))]
    assert command[1:3] == ["tools/ios_build.py", "--native-mac"]
    assert command[command.index("--data-pack-channel") + 1] == "tester"
    assert command[command.index("--signing-profile") + 1] == "MAC-UUID"
    assert command[command.index("--bundle-id") + 1] == "org.steverice.visr"
    assert command[command.index("--shader-feed-url") + 1] == policy["shader_feed_url"]
    for absent in ("--tester", "--extended-virtual-addressing", "--benchmark-upload", "--upload", "--macos"):
        assert absent not in command
    assert command[-2:] == ["--app-store-package", "build/release/macos"]


def test_ios_family_release_build_keeps_its_tester_flags(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("APPLE_TEAM_ID", "TEAM")
    monkeypatch.setenv("APPLE_BUNDLE_ID", "org.steverice.visr")
    policy = asset_policy()
    command = [str(part) for part in release.platform_build_command(
        "tvos", manifest(policy), policy, "TV-UUID", Path("pkg"))]
    for flag in ("--tester", "--extended-virtual-addressing", "--internal-only", "--tvos"):
        assert flag in command
    assert "--native-mac" not in command


def mac_bundle(app: Path, info: dict[str, Any]) -> None:
    (app / "Contents/MacOS").mkdir(parents=True)
    (app / "Contents/MacOS/VISR").write_bytes(b"mac executable")
    (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))


def test_mac_archive_identity_reads_the_bundle_contents(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    pins = asset_policy()
    guest = b"guest fixture"
    pins["guest_sha256"] = hashlib.sha256(guest).hexdigest()
    root = tmp_path / "source"
    (root / "build/ios").mkdir(parents=True)
    (root / "build/ios/halo_guest.elf").write_bytes(guest)
    archive = tmp_path / "VISR.xcarchive"
    info = {
        "ITSAppUsesNonExemptEncryption": False,
        "CFBundleShortVersionString": "0.5.0",
        "CFBundleVersion": "20261023.1",
        "DTPlatformName": "macosx",
        "DTXcodeBuild": release.XCODE_BUILDS[0],
        "DTSDKName": "macosx27.2",
        "CFBundleIdentifier": "org.steverice.visr",
        "CFBundleExecutable": "VISR",
        "HaloDataPackPublicKey": validation.DATA_PACK_PUBLIC_KEY,
        "HaloProfileFeedURL": pins["profile_feed_url"],
        "HaloRecordingFeedURL": pins["recording_feed_url"],
        "HaloShaderFeedURL": pins["shader_feed_url"],
        "HaloDataPackChannel": "tester",
    }
    mac_bundle(archive / "Products/Applications/VISR.app", info)
    checked = []
    monkeypatch.setattr(release, "verify_guest", lambda executable, *_: checked.append(executable))
    monkeypatch.setenv("APPLE_BUNDLE_ID", "org.steverice.visr")
    identity = release.archive_identity(archive, manifest(pins), "macos", source_root=root)
    assert identity["guest_sha256"] == pins["guest_sha256"]
    assert checked == [b"mac executable"]
    # an iOS-family archive in the Mac slot is refused
    info["DTPlatformName"] = "iphoneos"
    (archive / "Products/Applications/VISR.app/Contents/Info.plist").write_bytes(plistlib.dumps(info))
    with pytest.raises(ValueError, match="platform"):
        release.archive_identity(archive, manifest(pins), "macos", source_root=root)


def inspect_fixture(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, definitions: str) -> None:
    (tmp_path / "pgo").mkdir()
    (tmp_path / "pgo/halo_linux.profdata").write_bytes(b"guest-profile")
    (tmp_path / "pgo/visr_host.profdata").write_bytes(b"host-profile")
    (tmp_path / "build/ios").mkdir(parents=True)
    (tmp_path / "build/ios/halo_guest.elf").write_bytes(b"guest")
    mac_bundle(tmp_path / "build/macos/app-native/host/Release/VISR.app",
               {"CFBundleExecutable": "VISR", "DTXcodeBuild": release.XCODE_BUILDS[0], "DTSDKName": "macosx27.2"})
    monkeypatch.setattr(validation.release, "verify_guest", lambda *_: None)
    host_flags = "-fprofile-instr-use=" + str(tmp_path / "pgo/visr_host.profdata")

    def run(*args: Any, **kwargs: Any) -> str:
        if args[:2] == ("ninja", "-t"):
            return ("/opt/llvm/bin/clang -flto=full -fprofile-use=pgo/halo_linux.profdata -c "
                    "source/game/player_control.c\npython tools/ios_guest_lto.py")
        if args[0] == "xcodebuild":
            assert Path(args[2]).name == "VISRNative.xcodeproj"
            return json.dumps([{"target": "VISR", "buildSettings": {
                "OTHER_CFLAGS": host_flags, "GCC_PREPROCESSOR_DEFINITIONS": definitions}}])
        return "Homebrew clang version 23.1.2"

    monkeypatch.setattr(validation, "run", run)


def test_mac_probe_inspects_the_native_build_without_a_tester_build(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    inspect_fixture(tmp_path, monkeypatch, "HALO_NATIVE_MAC=1 HALO_BENCHMARK_NO_ROUTES=1")
    evidence = validation.inspect_build(tmp_path, "macos")
    assert evidence["tester"] == "off"
    assert evidence["benchmark_upload"] == "off"
    assert evidence["guest_sha256"] == hashlib.sha256(b"guest").hexdigest()
    with pytest.raises(ValueError, match="benchmark upload"):
        validation.inspect_build(tmp_path, "macos", benchmark_upload=True)


def test_mac_probe_refuses_a_tester_or_non_native_build(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    inspect_fixture(tmp_path, monkeypatch, "HALO_NATIVE_MAC=1 HALO_TESTER_BUILD=1 HALO_BENCHMARK_NO_ROUTES=1")
    with pytest.raises(ValueError, match="native Mac"):
        validation.inspect_build(tmp_path, "macos")


def test_mac_probe_build_is_ad_hoc_native_without_feeds() -> None:
    command = validation.build_command("macos", benchmark_upload=True)
    assert "--native-mac" in command
    for absent in ("--tester", "--unsigned", "--benchmark-upload", "--extended-virtual-addressing"):
        assert absent not in command
    assert command[command.index("--data-packs") + 1] == "off"


def test_mac_evidence_has_no_benchmark_upload_whatever_the_policy() -> None:
    policy = {"guest_sha256": "a", "guest_compiler_version": "b", "guest_profile_sha256": "c",
              "host_profile_sha256": "d", "benchmark_upload": "on"}
    evidence = {name: policy[name] for name in list(policy)[:4]}
    validation.compare_evidence({**evidence, "benchmark_upload": "off"}, policy, platform="macos")
    with pytest.raises(ValueError, match="benchmark-upload"):
        validation.compare_evidence({**evidence, "benchmark_upload": "off"}, policy, platform="ios")


def test_mac_release_needs_its_profile_and_the_installer_certificate() -> None:
    names = validation.credential_names(["ios", "macos"])
    for name in ("APPLE_PROFILE_MACOS_BASE64", "APPLE_INSTALLER_P12_BASE64", "APPLE_INSTALLER_P12_PASSWORD"):
        assert name in names
    assert "APPLE_INSTALLER_P12_BASE64" not in validation.credential_names(["ios", "tvos"])


def test_mac_export_must_be_one_signed_installer_package(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    with pytest.raises(ValueError, match="exactly one"):
        validation.verify_exported_pkg(tmp_path)
    (tmp_path / "VISR.pkg").write_bytes(b"pkg")
    output = "Status: signed by a certificate trusted by macOS\n    1. 3rd Party Mac Developer Installer: Example (TEAM)"
    monkeypatch.setattr(validation, "run", lambda *args, **kwargs: output)
    validation.verify_exported_pkg(tmp_path)
    monkeypatch.setattr(validation, "run", lambda *args, **kwargs: "Status: no signature")
    with pytest.raises(ValueError, match="installer certificate"):
        validation.verify_exported_pkg(tmp_path)


def test_mac_profile_must_allow_the_declared_icloud_container() -> None:
    allowed = {"Entitlements": {"com.apple.developer.icloud-services": ["CloudKit"],
                                "com.apple.developer.icloud-container-identifiers": ["iCloud.org.steverice.visr"]}}
    release_signing.validate_profile_entitlements(allowed, "macos")
    with pytest.raises(release_signing.ProfileCapabilityError, match="CloudKit"):
        release_signing.validate_profile_entitlements({"Entitlements": {}}, "macos")
    assert release_signing.profile_extension("macos") == ".provisionprofile"
    assert release_signing.profile_extension("ios") == ".mobileprovision"


def test_release_workflow_passes_mac_signing_secrets_to_both_signing_steps() -> None:
    workflow = (release.ROOT / ".github/workflows/release.yml").read_text()
    for name in ("APPLE_PROFILE_MACOS_BASE64", "APPLE_INSTALLER_P12_BASE64", "APPLE_INSTALLER_P12_PASSWORD"):
        assert workflow.count(f"{name}: ${{{{ secrets.{name} }}}}") == 2
