"""Fail-closed release build policy and validation-only build contracts."""

from __future__ import annotations

import base64
import hashlib
import json
import os
import plistlib
import re
import subprocess
from pathlib import Path
from typing import Any

import pytest
from cryptography.hazmat.primitives.asymmetric import ed25519 as crypto

from tools import release_validation as validation


def policy() -> dict[str, str]:
    return {
        "guest_sha256": "a" * 64,
        "guest_compiler_version": "Homebrew clang version 23.1.2",
        "guest_profile_sha256": "b" * 64,
        "host_profile_sha256": "c" * 64,
        "profile_feed_url": "https://example.invalid/tester/profiles-2.json",
        "profile_feed_sha256": "d" * 64,
        "recording_feed_url": "https://example.invalid/tester/recordings-"
        + "a" * 64
        + ".json",
        "recording_feed_sha256": "e" * 64,
    }


def recording_files() -> dict[str, str]:
    files = {
        "input": "halo input recording 1\n0 0000 0 0 0 0 0 0 0 0 0 0 0 0\n",
        "actions": "halo action recording 1\n0 0 00000000 00000000 00000000 00000000 80000000 00000000 ffffffff 0000ffff\n",
        "seeds": "halo seed recording 1\n0 0 00000000 12345678\n",
        "route": "map = a10\ndifficulty = normal\nentered = start\nfirst_tick = 0\nplayers = 1\n",
    }
    return {
        "test-route." + name: base64.b64encode(data.encode()).decode()
        for name, data in files.items()
    }


def test_policy_requires_exact_guest_and_explicit_https_feeds() -> None:
    assert validation.load_policy(json.dumps(policy())) == policy()
    for field, value in [
        ("guest_sha256", ""),
        ("profile_feed_url", "http://example.invalid/feed"),
        ("recording_feed_sha256", "bad"),
        ("guest_compiler_version", ""),
    ]:
        invalid = {**policy(), field: value}
        with pytest.raises(ValueError):
            validation.load_policy(json.dumps(invalid))


def test_missing_signing_values_fail_before_any_external_command() -> None:
    with pytest.raises(ValueError, match="APPLE_TEAM_ID"):
        validation.check_credentials({}, ["ios", "tvos"])
    names = validation.credential_names(["ios"])
    env = dict.fromkeys(names, "fixture")
    validation.check_credentials(env, ["ios"])
    with pytest.raises(ValueError, match="APPLE_PROFILE_TVOS_BASE64"):
        validation.check_credentials(env, ["ios", "tvos"])


def test_probe_command_is_unsigned_and_has_explicit_optimized_tester_policy() -> None:
    command = validation.build_command("tvos")
    for name, value in [
        ("--benchmark-routes", "off"),
        ("--pgo", "use"),
        ("--lto", "full"),
        ("--host-pgo", "use"),
        ("--render-validation", "off"),
        ("--data-packs", "off"),
    ]:
        assert command[command.index(name) + 1] == value
    assert "--unsigned" in command and "--tester" in command and "--tvos" in command
    assert not {"--upload", "--team", "--app-store-package", "--register-device"} & set(
        command
    )


def test_feed_rejects_changed_bytes_and_wrong_guest() -> None:
    payload = b'{"files":{}}'
    manifest = {
        "schema": 1,
        "sequence": 1,
        "notes": "",
        "kind": "recordings",
        "channel": "tester",
        "compatibility": "benchmark-replay-1:" + "f" * 64,
        "minimum_engine": 1,
        "maximum_engine": 1,
        "sha256": hashlib.sha256(payload).hexdigest(),
    }
    envelope = json.dumps(
        {
            "manifest": base64.b64encode(json.dumps(manifest).encode()).decode(),
            "signature": base64.b64encode(bytes(64)).decode(),
            "payload": base64.b64encode(payload).decode(),
        }
    ).encode()
    pins = {**policy(), "recording_feed_sha256": hashlib.sha256(envelope).hexdigest()}
    with pytest.raises(ValueError, match="compatibility"):
        validation.verify_feed(envelope, "recording", pins)
    with pytest.raises(ValueError, match="hash"):
        validation.verify_feed(envelope + b" ", "recording", pins)


def test_probe_never_calls_release_mutation_helpers(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    calls: list[tuple[Any, ...]] = []
    source = "1" * 40

    def run(*args: Any, **kwargs: Any) -> str:
        calls.append(args)
        if args[:3] == ("git", "rev-parse", "HEAD"):
            return source
        if args[:3] == ("git", "show", "--no-patch"):
            return "1760576523"
        return ""

    monkeypatch.setattr(validation, "run", run)
    monkeypatch.setattr(validation, "toolchain", lambda *_: {"xcode_build": "27B5028f"})
    monkeypatch.setattr(
        validation, "inspect_build", lambda *_: {"guest_sha256": "a" * 64}
    )
    report = validation.probe(tmp_path, source, ["ios", "tvos"])
    assert (
        report["source_sha"] == source and report["source_date_epoch"] == "1760576523"
    )
    assert (
        report["platforms"]["ios"]["guest_sha256"]
        == report["platforms"]["tvos"]["guest_sha256"]
    )
    assert not any(
        args[:2] in [("git", "push"), ("git", "tag"), ("cz", "bump")] for args in calls
    )
    assert all("release.py" not in args for args in calls)


def test_probe_rejects_different_platform_guests(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = "1" * 40
    monkeypatch.setattr(
        validation,
        "run",
        lambda *args, **_: source if "rev-parse" in args else "1760576523",
    )
    monkeypatch.setattr(validation, "toolchain", lambda *_: {})
    monkeypatch.setattr(
        validation,
        "inspect_build",
        lambda root, platform, *_: {"guest_sha256": platform},
    )
    with pytest.raises(ValueError, match="same guest"):
        validation.probe(tmp_path, source, ["ios", "tvos"])


def test_effective_compile_flags_reject_host_pgo_fallback(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    (tmp_path / "pgo").mkdir()
    (tmp_path / "pgo/halo_linux.profdata").write_bytes(b"guest-profile")
    (tmp_path / "pgo/visr_host.profdata").write_bytes(b"host-profile")
    (tmp_path / "build/ios").mkdir(parents=True)
    (tmp_path / "build/ios/halo_guest.elf").write_bytes(b"guest")
    app = tmp_path / "build/ios/app-unsigned/Release-iphoneos/VISR.app"
    app.mkdir(parents=True)
    (app / "Info.plist").write_bytes(
        plistlib.dumps(
            {
                "CFBundleExecutable": "VISR",
                "DTXcodeBuild": "27B5028f",
                "DTSDKName": "iphoneos27.2",
            }
        )
    )
    (app / "VISR").write_bytes(b"unsigned-executable")
    monkeypatch.setattr(validation.release, "verify_guest", lambda *_: None)
    host_flags = ""
    upload_enabled = False
    codesign_calls = []
    def run(*args: Any, **kwargs: Any) -> str:
        if args[:2] == ("ninja", "-t"):
            return "/opt/llvm/bin/clang -flto=full -fprofile-use=pgo/halo_linux.profdata -c source/game/player_control.c\npython tools/ios_guest_lto.py"
        if args[0] == "codesign":
            codesign_calls.append(args)
            return plistlib.dumps(
                {
                    "com.apple.developer.icloud-services": ["CloudKit"],
                    "com.apple.developer.icloud-container-identifiers": ["iCloud.org.steverice.visr"],
                    "com.apple.developer.icloud-container-environment": "Production",
                    "aps-environment": "production",
                }
            ).decode()
        if args[0] == "xcodebuild":
            return json.dumps(
                [
                    {
                        "target": "VISR",
                        "buildSettings": {
                            "OTHER_CFLAGS": host_flags,
                            "GCC_PREPROCESSOR_DEFINITIONS": "HALO_TESTER_BUILD=1 HALO_BENCHMARK_NO_ROUTES=1" + (" HALO_BENCHMARK_UPLOAD=1" if upload_enabled else ""),
                        },
                    }
                ]
            )
        return "Homebrew clang version 23.1.2"

    monkeypatch.setattr(validation, "run", run)
    with pytest.raises(ValueError, match="host PGO"):
        validation.inspect_build(tmp_path, "ios")
    host_flags = "-fprofile-instr-use=" + str(tmp_path / "pgo/visr_host.profdata")
    result = validation.inspect_build(tmp_path, "ios")
    upload_enabled = True
    unsigned_probe = validation.inspect_build(tmp_path, "ios", benchmark_upload=True)
    assert unsigned_probe["benchmark_upload"] == "on"
    assert not codesign_calls
    signed_archive = validation.inspect_build(
        tmp_path, "ios", app=app, benchmark_upload=True
    )
    assert signed_archive["benchmark_upload"] == "on"
    assert len(codesign_calls) == 1
    assert result["guest_sha256"] == hashlib.sha256(b"guest").hexdigest()
    assert (
        result["guest_profile_sha256"] == hashlib.sha256(b"guest-profile").hexdigest()
    )


def test_preflight_identity_changes_with_source_platforms_or_policy() -> None:
    original = validation.preflight_identity("1" * 40, ["ios", "tvos"], policy())
    assert original != validation.preflight_identity(
        "2" * 40, ["ios", "tvos"], policy()
    )
    assert original != validation.preflight_identity("1" * 40, ["ios"], policy())
    assert original != validation.preflight_identity(
        "1" * 40, ["ios", "tvos"], {**policy(), "guest_sha256": "f" * 64}
    )


def test_signed_feed_verifies_signature_and_payload(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

    key = crypto.Ed25519PrivateKey.generate()
    monkeypatch.setattr(
        validation,
        "DATA_PACK_PUBLIC_KEY",
        key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex(),
    )
    payload = json.dumps({"files": recording_files()}).encode()
    manifest = json.dumps(
        {
            "schema": 1,
            "sequence": 1,
            "notes": "💥" * 2048,
            "kind": "recordings",
            "channel": "tester",
            "minimum_engine": 1,
            "maximum_engine": 1,
            "compatibility": "benchmark-replay-1:" + "a" * 64,
            "sha256": hashlib.sha256(payload).hexdigest(),
        }
    ).encode()

    def envelope(signature: bytes, body: bytes = payload) -> bytes:
        return json.dumps(
            {
                name: base64.b64encode(value).decode()
                for name, value in {
                    "manifest": manifest,
                    "signature": signature,
                    "payload": body,
                }.items()
            }
        ).encode()

    data = envelope(key.sign(manifest))
    pins = {**policy(), "recording_feed_sha256": hashlib.sha256(data).hexdigest()}
    validation.verify_feed(data, "recording", pins)
    bad = envelope(b"x" * 64)
    with pytest.raises(InvalidSignature):
        validation.verify_feed(
            bad,
            "recording",
            {**pins, "recording_feed_sha256": hashlib.sha256(bad).hexdigest()},
        )
    bad = envelope(key.sign(manifest), b"altered")
    with pytest.raises(ValueError, match="payload hash"):
        validation.verify_feed(
            bad,
            "recording",
            {**pins, "recording_feed_sha256": hashlib.sha256(bad).hexdigest()},
        )


def test_rejected_source_never_runs_secret_job_cleanup(tmp_path: Path) -> None:
    """Model GitHub's failure/default/always conditions with a real rejected Git source."""
    workflow = (validation.release.ROOT / ".github/workflows/release.yml").read_text()
    preflight = workflow.split("\n  preflight:\n", 1)[1].split("\n  prepare:\n", 1)[0]
    guard = preflight.split(
        "- name: Verify source is on main before using credentials", 1
    )[1].split("\n      - ", 1)[0]
    cleanup = preflight.split("- name: Remove signing material", 1)[1]
    condition = re.search(r"^        if: (.+)$", cleanup, re.MULTILINE)
    assert condition is not None

    def git(*args: str) -> str:
        return subprocess.check_output(
            ["git", *args], cwd=tmp_path, text=True, stderr=subprocess.PIPE
        ).strip()

    git("init", "--initial-branch=main")
    git("config", "user.name", "Fixture")
    git("config", "user.email", "fixture@example.invalid")
    script = tmp_path / "tools/release_signing.py"
    script.parent.mkdir()
    script.write_text(
        'import os\nfrom pathlib import Path\nPath(os.environ["PROBE_MARKER"]).unlink(missing_ok=True)\n'
    )
    git("add", "--", "tools/release_signing.py")
    git("commit", "--message", "trusted fixture")
    trusted = git("rev-parse", "HEAD")
    git("update-ref", "refs/remotes/origin/main", trusted)
    script.write_text(
        'import os\nfrom pathlib import Path\nPath(os.environ["PROBE_MARKER"]).write_text(os.environ["ASC_PRIVATE_KEY"])\n'
    )
    git("add", "--", "tools/release_signing.py")
    git("commit", "--message", "untrusted fixture")
    rejected = git("rev-parse", "HEAD")
    marker = tmp_path / "secret-executed"
    env = {
        "PATH": os.environ["PATH"],
        "SOURCE_SHA": rejected,
        "ASC_PRIVATE_KEY": "fixture-secret",
        "PROBE_MARKER": str(marker),
    }
    command = re.search(r"^        run: (.+)$", guard, re.MULTILINE)
    assert command is not None
    result = subprocess.run(
        command.group(1),
        cwd=tmp_path,
        env=env,
        shell=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode != 0  # This source is not on the protected main history.
    # GitHub's always() ignores prior failure unless the cleanup has the explicit trust gate.
    if condition.group(1) == "always()":
        subprocess.run(
            [validation.sys.executable, str(script), "cleanup"],
            cwd=tmp_path,
            env=env,
            check=True,
        )
    else:
        gate = re.fullmatch(
            r"always\(\) && steps\.([a-z-]+)\.outcome == 'success'", condition.group(1)
        )
        assert gate is not None
        assert f"id: {gate.group(1)}" in guard
    assert not marker.exists(), (
        "Rejected source executed repository cleanup with release secrets"
    )


def test_trusted_source_cleanup_still_runs_after_signing_failure() -> None:
    workflow = (validation.release.ROOT / ".github/workflows/release.yml").read_text()
    preflight = workflow.split("\n  preflight:\n", 1)[1].split("\n  prepare:\n", 1)[0]
    cleanup = preflight.split("- name: Remove signing material", 1)[1]
    condition = re.search(r"^        if: (.+)$", cleanup, re.MULTILINE)
    assert condition is not None
    assert condition.group(1) == "always() && steps.trusted-source.outcome == 'success'"
    assert "if: success()" not in cleanup
    assert preflight.index("id: trusted-source") < preflight.index(
        "tools/release_signing.py install"
    )


@pytest.mark.parametrize(
    "fault",
    [
        "missing-sequence",
        "missing-notes",
        "boolean-sequence",
        "excess-sequence",
        "wrong-notes-type",
        "empty-files",
        "unexpected-payload-field",
        "unsafe-name",
        "noncanonical-base64",
        "invalid-recording-content",
    ],
)
def test_signed_feed_must_pass_the_app_manifest_and_snapshot_contract(
    fault: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

    key = crypto.Ed25519PrivateKey.generate()
    monkeypatch.setattr(
        validation,
        "DATA_PACK_PUBLIC_KEY",
        key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex(),
    )
    payload_value: dict[str, Any] = {"files": recording_files()}
    manifest: dict[str, Any] = {
        "schema": 1,
        "kind": "recordings",
        "channel": "tester",
        "sequence": 1,
        "minimum_engine": 1,
        "maximum_engine": 1,
        "compatibility": "benchmark-replay-1:" + "a" * 64,
        "notes": "",
    }
    if fault == "missing-sequence":
        del manifest["sequence"]
    elif fault == "missing-notes":
        del manifest["notes"]
    elif fault == "boolean-sequence":
        manifest["sequence"] = True
    elif fault == "excess-sequence":
        manifest["sequence"] = 2**53
    elif fault == "wrong-notes-type":
        manifest["notes"] = 17
    elif fault == "empty-files":
        payload_value["files"] = {}
    elif fault == "unexpected-payload-field":
        payload_value["extra"] = 1
    elif fault == "unsafe-name":
        payload_value["files"]["../escape.input"] = base64.b64encode(b"x").decode()
    elif fault == "noncanonical-base64":
        payload_value["files"]["test-route.input"] = "eA==" + "\n"
    else:
        payload_value["files"]["test-route.input"] = base64.b64encode(
            b"not a recording"
        ).decode()
    payload = json.dumps(payload_value).encode()
    manifest["sha256"] = hashlib.sha256(payload).hexdigest()
    raw = json.dumps(manifest).encode()
    data = json.dumps(
        {
            name: base64.b64encode(value).decode()
            for name, value in {
                "manifest": raw,
                "signature": key.sign(raw),
                "payload": payload,
            }.items()
        }
    ).encode()
    pins = {**policy(), "recording_feed_sha256": hashlib.sha256(data).hexdigest()}
    with pytest.raises(ValueError):
        validation.verify_feed(data, "recording", pins)


@pytest.mark.parametrize(
    "url",
    [
        "https://example.invalid/tester/feed.json?channel=tester",
        "https://example.invalid/tester/a b.json",
    ],
)
def test_release_policy_rejects_feed_urls_the_archive_builder_refuses(url: str) -> None:
    for field in ("profile_feed_url", "recording_feed_url"):
        with pytest.raises(ValueError, match="feed URL"):
            validation.load_policy(json.dumps({**policy(), field: url}))


@pytest.mark.parametrize("field", ["profile_feed_url", "recording_feed_url"])
@pytest.mark.parametrize("url", [
    "https://raw.githubusercontent.com/example/app/" + "a" * 40 + "/packs/feed.json",
    "https://raw.github.com/example/app/" + "A" * 40 + "/packs/feed.json",
    "https://github.com/example/app/raw/" + "a" * 40 + "/packs/feed.json",
    "https://github.com/example/app/blob/" + "a" * 40 + "/packs/feed.json",
    "https://raw.githubusercontent.com/example/app/" + "%61" * 40 + "/packs/feed.json",
])
def test_runtime_feed_rejects_commit_pinned_github(field: str, url: str) -> None:
    with pytest.raises(ValueError, match="runtime feed URL is commit-pinned"):
        validation.load_policy(json.dumps({**policy(), field: url}))


@pytest.mark.parametrize("url", [
    "https://code.steverice.org/visr/packs/tester/recordings-" + "a" * 64 + ".json",
    "https://raw.githubusercontent.com/example/app/data/packs/feed.json",
    "https://raw.githubusercontent.com/example/app/refs/heads/data/packs/feed.json",
    "https://updates.example.invalid/" + "a" * 40 + "/recordings.json",
])
def test_mutable_and_custom_runtime_feeds_keep_the_exact_release_hash(url: str) -> None:
    pins = {**policy(), "recording_feed_url": url}
    assert validation.load_policy(json.dumps(pins)) == pins


@pytest.mark.parametrize(
    "raw",
    [
        b'{"schema":1,"schema":1}',
        b'{"schema":1,"\\u0073chema":1}',
        b'{"files":{"a":{},"b":[]}}',
        b'{"files":{"nested":{"deep":1}}}',
        b'{"files":NaN}',
    ],
)
def test_feed_json_matches_the_runtime_unique_key_and_shape_rules(raw: bytes) -> None:
    with pytest.raises((ValueError, TypeError)):
        validation.packs.decode_json(raw)


def test_benchmark_upload_policy_defaults_off_and_preserves_legacy_policy() -> None:
    legacy = policy()
    assert validation.load_policy(json.dumps(legacy)) == legacy
    assert validation.benchmark_upload_enabled(legacy) is False

    enabled = {**legacy, "benchmark_upload": "on"}
    disabled = {**legacy, "benchmark_upload": "off"}
    assert validation.load_policy(json.dumps(enabled)) == enabled
    assert validation.benchmark_upload_enabled(enabled) is True
    assert validation.load_policy(json.dumps(disabled)) == disabled
    assert validation.benchmark_upload_enabled(disabled) is False

    with pytest.raises(ValueError, match="benchmark_upload"):
        validation.load_policy(json.dumps({**legacy, "benchmark_upload": "yes"}))


def test_benchmark_probe_build_includes_upload_only_when_policy_enables_it() -> None:
    assert "--benchmark-upload" not in validation.build_command("ios")
    enabled = validation.build_command("tvos", benchmark_upload=True)
    assert "--benchmark-upload" in enabled
    assert enabled[enabled.index("--unsigned") + 1] == "--tester"


def test_exported_ipa_uses_the_production_cloudkit_entitlements(tmp_path, monkeypatch):
    import zipfile

    package = tmp_path / "package"
    package.mkdir()
    with zipfile.ZipFile(package / "VISR.ipa", "w") as exported:
        exported.writestr("Payload/VISR.app/Info.plist", plistlib.dumps({"CFBundleIdentifier": "org.steverice.visr"}))
    entitlements = {
        "com.apple.developer.icloud-services": ["CloudKit"],
        "com.apple.developer.icloud-container-identifiers": ["iCloud.org.steverice.visr"],
        "com.apple.developer.icloud-container-environment": "Production",
        "aps-environment": "production",
    }
    monkeypatch.setattr(validation, "run", lambda *args, **kwargs: plistlib.dumps(entitlements).decode())
    validation.verify_exported_ipa(package)


def test_exported_ipa_rejects_development_aps(tmp_path, monkeypatch):
    import zipfile

    package = tmp_path / "package"
    package.mkdir()
    with zipfile.ZipFile(package / "VISR.ipa", "w") as exported:
        exported.writestr("Payload/VISR.app/Info.plist", plistlib.dumps({"CFBundleIdentifier": "org.steverice.visr"}))
    entitlements = {
        "com.apple.developer.icloud-services": ["CloudKit"],
        "com.apple.developer.icloud-container-identifiers": ["iCloud.org.steverice.visr"],
        "com.apple.developer.icloud-container-environment": "Production",
        "aps-environment": "development",
    }
    monkeypatch.setattr(validation, "run", lambda *args, **kwargs: plistlib.dumps(entitlements).decode())
    with pytest.raises(ValueError, match="production APS"):
        validation.verify_exported_ipa(package)


@pytest.mark.parametrize("cloudkit_environment", ["Development", None])
def test_exported_ipa_rejects_nonproduction_cloudkit_environment(tmp_path, monkeypatch, cloudkit_environment):
    import zipfile

    package = tmp_path / "package"
    package.mkdir()
    with zipfile.ZipFile(package / "VISR.ipa", "w") as exported:
        exported.writestr("Payload/VISR.app/Info.plist", plistlib.dumps({"CFBundleIdentifier": "org.steverice.visr"}))
    entitlements = {
        "com.apple.developer.icloud-services": ["CloudKit"],
        "com.apple.developer.icloud-container-identifiers": ["iCloud.org.steverice.visr"],
        "aps-environment": "production",
    }
    if cloudkit_environment is not None:
        entitlements["com.apple.developer.icloud-container-environment"] = cloudkit_environment
    monkeypatch.setattr(validation, "run", lambda *args, **kwargs: plistlib.dumps(entitlements).decode())
    with pytest.raises(ValueError, match="production CloudKit environment"):
        validation.verify_exported_ipa(package)
