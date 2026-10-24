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
from typing import Any, Self

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
    monkeypatch.setattr(validation, "toolchain", lambda *_: {"xcode_build": "27B5038j"})
    monkeypatch.setattr(
        validation, "inspect_build", lambda *_, **__: {"guest_sha256": "a" * 64}
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
        lambda root, platform, *_, **__: {"guest_sha256": platform},
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
                "DTXcodeBuild": "27B5038j",
                "DTSDKName": "iphoneos27.2",
            }
        )
    )
    (app / "VISR").write_bytes(b"unsigned-executable")
    monkeypatch.setattr(validation.release, "verify_guest", lambda *_: None)
    host_flags = ""
    upload_enabled = False
    saves_enabled = False
    codesign_calls = []

    def run(*args: Any, **kwargs: Any) -> str:
        if args[:2] == ("ninja", "-t"):
            return "/opt/llvm/bin/clang -flto=full -fprofile-use=pgo/halo_linux.profdata -c source/game/player_control.c\npython tools/ios_guest_lto.py"
        if args[0] == "codesign":
            codesign_calls.append(args)
            return plistlib.dumps(
                {
                    "com.apple.developer.icloud-services": ["CloudKit"],
                    "com.apple.developer.icloud-container-identifiers": [
                        "iCloud.org.steverice.visr"
                    ],
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
                            "GCC_PREPROCESSOR_DEFINITIONS": "HALO_TESTER_BUILD=1 HALO_BENCHMARK_NO_ROUTES=1"
                            + (" HALO_BENCHMARK_UPLOAD=1" if upload_enabled else "")
                            + (" HALO_CLOUD_SAVES=1" if saves_enabled else ""),
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
    saves_enabled = True
    with pytest.raises(ValueError, match="cloud-saves"):
        validation.inspect_build(tmp_path, "ios")
    saves_probe = validation.inspect_build(tmp_path, "ios", cloud_saves=True)
    assert saves_probe["cloud_saves"] == "on"
    saves_archive = validation.inspect_build(tmp_path, "ios", app=app, cloud_saves=True)
    assert saves_archive["cloud_saves"] == "on"
    codesign_calls.clear()
    saves_enabled = False
    with pytest.raises(ValueError, match="cloud-saves"):
        validation.inspect_build(tmp_path, "ios", cloud_saves=True)
    upload_enabled = True
    unsigned_probe = validation.inspect_build(tmp_path, "ios", benchmark_upload=True)
    assert unsigned_probe["benchmark_upload"] == "on"
    assert not codesign_calls
    signed_archive = validation.inspect_build(
        tmp_path, "ios", app=app, benchmark_upload=True
    )
    assert signed_archive["benchmark_upload"] == "on"
    (app / "VISR").write_bytes(b"executable " + validation.LOCAL_CATALOG_NOTE)
    with pytest.raises(ValueError, match="unsigned local benchmark catalog"):
        validation.inspect_build(tmp_path, "ios", benchmark_upload=True)
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
    tmp_path: Path,
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
    from tools import data_pack
    from tools.test_benchmark_catalog import _cases, _recording, _write_catalog

    _recording(tmp_path)
    _write_catalog(tmp_path, _cases())
    compatibility = "benchmark-catalog-1:" + "a" * 64
    files = data_pack.snapshot(tmp_path, "recordings", compatibility)
    payload = json.dumps(
        {
            "files": {
                name: base64.b64encode(content).decode()
                for name, content in files.items()
            }
        }
    ).encode()
    manifest = json.dumps(
        {
            "schema": 1,
            "sequence": 1,
            "notes": "💥" * 2048,
            "kind": "recordings",
            "channel": "tester",
            "minimum_engine": 1,
            "maximum_engine": 1,
            "compatibility": compatibility,
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


def test_signed_catalog_feed_uses_the_app_contract(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

    from tools import data_pack
    from tools.test_benchmark_catalog import _cases, _recording, _write_catalog

    key = crypto.Ed25519PrivateKey.generate()
    monkeypatch.setattr(
        validation,
        "DATA_PACK_PUBLIC_KEY",
        key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex(),
    )
    _recording(tmp_path)
    cases = _cases(players=16, gameplay=False)
    cases["mp-chillout-proof"]["order"] = 2
    cases["mp-bloodgulch-proof"] = {
        "kind": "multiplayer",
        "order": 3,
        "map": "bloodgulch",
        "difficulty": "normal",
        "recording": None,
        "lead": 15,
        "window": 60,
        "simulation": "network_players",
        "players": 32,
        "views": 1,
        "game_mode": "slayer",
        "score_limit": 1000,
        "time_limit_minutes": 0,
        "arena": {
            "columns": 6,
            "spacing_milliunits": 1500,
            "distance_milliunits": 3500,
            "movement_radius_milliunits": 400,
        },
    }
    _write_catalog(tmp_path, cases)
    compatibility = "benchmark-catalog-1:" + "a" * 64
    files = data_pack.snapshot(tmp_path, "recordings", compatibility)
    payload = json.dumps(
        {
            "files": {
                name: base64.b64encode(content).decode()
                for name, content in files.items()
            }
        },
        sort_keys=True,
        separators=(",", ":"),
    ).encode()

    def signed_feed(contract: str, payload: bytes) -> bytes:
        manifest = json.dumps(
            {
                "schema": 1,
                "sequence": 1,
                "notes": "qualified catalog",
                "kind": "recordings",
                "channel": "tester",
                "minimum_engine": 1,
                "maximum_engine": 1,
                "compatibility": contract,
                "sha256": hashlib.sha256(payload).hexdigest(),
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode()
        return json.dumps(
            {
                name: base64.b64encode(value).decode()
                for name, value in {
                    "manifest": manifest,
                    "signature": key.sign(manifest),
                    "payload": payload,
                }.items()
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode()

    catalog = signed_feed(compatibility, payload)
    pins = {**policy(), "recording_feed_sha256": hashlib.sha256(catalog).hexdigest()}
    summary_context = validation.verify_feed(catalog, "recording", pins)
    assert summary_context is not None
    assert summary_context["guest_sha256"] == "a" * 64
    assert summary_context["envelope_sha256"] == pins["recording_feed_sha256"]
    multiplayer = [
        case for case in summary_context["cases"] if case["kind"] == "multiplayer"
    ]
    assert [(case["map"], case["players"]) for case in multiplayer] == [
        ("chillout", 16),
        ("bloodgulch", 32),
    ]
    assert all(
        set(case) == {"id", "kind", "map", "players"}
        for case in summary_context["cases"]
    )

    legacy_payload = json.dumps(
        {"files": recording_files()}, sort_keys=True, separators=(",", ":")
    ).encode()
    legacy = signed_feed("benchmark-replay-1:" + "a" * 64, legacy_payload)
    legacy_pins = {
        **policy(),
        "recording_feed_sha256": hashlib.sha256(legacy).hexdigest(),
    }
    with pytest.raises(ValueError, match="compatibility"):
        validation.verify_feed(legacy, "recording", legacy_pins)
    from tools.pages_recording_overlay import checked_feed

    assert checked_feed(legacy, "a" * 64) == 1
    assert checked_feed(catalog, "a" * 64) == 1
    with pytest.raises(ValueError, match="compatibility"):
        checked_feed(legacy, "b" * 64)


def test_check_feeds_returns_facts_for_the_exact_downloaded_recording_bytes(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    policy_values = policy()
    downloads = {
        policy_values["profile_feed_url"]: b"profile-envelope",
        policy_values["recording_feed_url"]: b"signed-recording-envelope",
    }
    calls: list[tuple[str, bytes]] = []
    summary = recording_context = {
        "schema": 1,
        "guest_sha256": policy_values["guest_sha256"],
        "envelope_sha256": policy_values["recording_feed_sha256"],
        "payload_sha256": "f" * 64,
        "catalog_sha256": "1" * 64,
        "compatibility": "benchmark-catalog-1:" + policy_values["guest_sha256"],
        "cases": [
            {
                "id": "mp-current",
                "kind": "multiplayer",
                "map": "bloodgulch",
                "players": 32,
            }
        ],
    }

    class Response:
        def __init__(self, url: str) -> None:
            self.url = url

        def __enter__(self) -> Self:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, limit: int) -> bytes:
            return downloads[self.url][:limit]

    class Opener:
        def open(self, url: str, timeout: int) -> Response:
            assert timeout == 45
            return Response(url)

    def verify(
        data: bytes, kind: str, _policy: dict[str, str]
    ) -> dict[str, Any] | None:
        calls.append((kind, data))
        return recording_context if kind == "recording" else None

    monkeypatch.setattr(
        validation.urllib.request, "build_opener", lambda *_args: Opener()
    )
    monkeypatch.setattr(validation, "verify_feed", verify)

    assert validation.check_feeds(policy_values) == summary
    assert calls == [
        ("profile", downloads[policy_values["profile_feed_url"]]),
        ("recording", downloads[policy_values["recording_feed_url"]]),
    ]


def test_release_workflow_passes_preflight_catalog_context_to_prepare() -> None:
    workflow = (validation.release.ROOT / ".github/workflows/release.yml").read_text()
    preflight = workflow.split("\n  preflight:\n", 1)[1].split("\n  prepare:\n", 1)[0]
    prepare = workflow.split("\n  prepare:\n", 1)[1].split("\n  testflight:\n", 1)[0]
    assert (
        "recording_context: ${{ steps.check.outputs.recording_context }}" in preflight
    )
    assert (
        "RELEASE_RECORDING_CONTEXT: ${{ needs.preflight.outputs.recording_context }}"
        in prepare
    )


def test_release_workflow_auth_mode_is_oidc_only_and_cannot_build_or_publish() -> None:
    workflow = (validation.release.ROOT / ".github/workflows/release.yml").read_text()
    assert "options: [validate, auth, release]" in workflow
    assert "source_sha:\n        description:" in workflow
    source_input = workflow.split("      source_sha:\n", 1)[1].split(
        "      platforms:\n", 1
    )[0]
    assert "required: false" in source_input
    jobs = workflow.split("\njobs:\n", 1)[1]
    auth = jobs.split("\n  identity-check:\n", 1)[1].split("\n  preflight:\n", 1)[0]
    probe = jobs.split("  probe:\n", 1)[1].split("\n  identity-check:\n", 1)[0]
    preflight = jobs.split("\n  preflight:\n", 1)[1].split("\n  prepare:\n", 1)[0]
    prepare = jobs.split("\n  prepare:\n", 1)[1].split("\n  testflight:\n", 1)[0]
    testflight = jobs.split("\n  testflight:\n", 1)[1]
    assert "github.event_name == 'workflow_dispatch'" in auth
    assert "inputs.mode == 'auth'" in auth
    assert "github.ref == 'refs/heads/main'" in auth
    assert "environment: release" in auth
    assert "contents: read" in auth
    assert "id-token: write" in auth
    assert "contents: write" not in auth
    assert "release.py identity-check" in auth
    assert "messages" not in auth.lower()
    assert "inputs.mode != 'auth'" in probe
    assert "inputs.mode == 'release'" in preflight
    assert "needs: preflight" in prepare and "contents: write" in prepare
    assert "id-token: write" in prepare
    assert "ANTHROPIC_API_KEY" not in prepare
    assert "needs: prepare" in testflight
    assert "id-token: write" not in testflight
    for variable in (
        "ANTHROPIC_FEDERATION_RULE_ID",
        "ANTHROPIC_ORGANIZATION_ID",
        "ANTHROPIC_SERVICE_ACCOUNT_ID",
        "ANTHROPIC_WORKSPACE_ID",
    ):
        assert variable in auth and variable in prepare


def test_check_command_emits_verified_catalog_context(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    context = {
        "schema": 1,
        "guest_sha256": policy()["guest_sha256"],
        "envelope_sha256": policy()["recording_feed_sha256"],
        "payload_sha256": "f" * 64,
        "catalog_sha256": "1" * 64,
        "compatibility": "benchmark-catalog-1:" + policy()["guest_sha256"],
        "cases": [
            {
                "id": "mp-current",
                "kind": "multiplayer",
                "map": "bloodgulch",
                "players": 32,
            }
        ],
    }
    source = "a" * 40
    report = tmp_path / "release-probe.json"
    report.write_text(json.dumps({"source_sha": source}))
    outputs: list[tuple[str, str]] = []
    monkeypatch.setattr(
        "sys.argv",
        [
            "release_validation.py",
            "check",
            "--source-sha",
            source,
            "--platforms",
            "ios",
            "--report",
            str(report),
        ],
    )
    monkeypatch.setattr(validation.release, "require_ci", lambda: None)
    monkeypatch.setattr(validation, "check_release", lambda *_args: context)
    monkeypatch.setattr(validation, "preflight_identity", lambda *_args: "identity")
    monkeypatch.setattr(validation.release, "API", lambda *_args: object())
    monkeypatch.setattr(validation.release, "exact_app", lambda *_args: {})
    monkeypatch.setattr(
        validation.release, "output", lambda name, value: outputs.append((name, value))
    )
    monkeypatch.setenv("APPLE_BUNDLE_ID", "org.example.visr")
    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(policy()))

    validation.main()

    assert outputs == [
        ("identity", "identity"),
        (
            "recording_context",
            json.dumps(context, sort_keys=True, separators=(",", ":")),
        ),
    ]


def test_pages_overlay_keeps_the_published_legacy_feed_unchanged() -> None:
    from tools.pages_recording_overlay import checked_feed

    guest = "4d6924ec86afba868b1fd994bcc323d441b18991ef5367cebca18a6b5cb97d2d"
    feed = (
        Path(__file__).resolve().parents[1]
        / "docs/site/packs/tester"
        / f"recordings-{guest}.json"
    )
    assert checked_feed(feed.read_bytes(), guest) == 3


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
@pytest.mark.parametrize(
    "url",
    [
        "https://raw.githubusercontent.com/example/app/"
        + "a" * 40
        + "/packs/feed.json",
        "https://raw.github.com/example/app/" + "A" * 40 + "/packs/feed.json",
        "https://github.com/example/app/raw/" + "a" * 40 + "/packs/feed.json",
        "https://github.com/example/app/blob/" + "a" * 40 + "/packs/feed.json",
        "https://raw.githubusercontent.com/example/app/"
        + "%61" * 40
        + "/packs/feed.json",
    ],
)
def test_runtime_feed_rejects_commit_pinned_github(field: str, url: str) -> None:
    with pytest.raises(ValueError, match="runtime feed URL is commit-pinned"):
        validation.load_policy(json.dumps({**policy(), field: url}))


@pytest.mark.parametrize(
    "url",
    [
        "https://code.steverice.org/visr/packs/tester/recordings-" + "a" * 64 + ".json",
        "https://raw.githubusercontent.com/example/app/data/packs/feed.json",
        "https://raw.githubusercontent.com/example/app/refs/heads/data/packs/feed.json",
        "https://updates.example.invalid/" + "a" * 40 + "/recordings.json",
    ],
)
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
        exported.writestr(
            "Payload/VISR.app/Info.plist",
            plistlib.dumps({
                "CFBundleIdentifier": "org.steverice.visr",
                "ITSAppUsesNonExemptEncryption": False,
            }),
        )
    entitlements = {
        "com.apple.developer.icloud-services": ["CloudKit"],
        "com.apple.developer.icloud-container-identifiers": [
            "iCloud.org.steverice.visr"
        ],
        "com.apple.developer.icloud-container-environment": "Production",
        "aps-environment": "production",
    }
    monkeypatch.setattr(
        validation, "run", lambda *args, **kwargs: plistlib.dumps(entitlements).decode()
    )
    validation.verify_exported_ipa(package)


def test_exported_ipa_rejects_development_aps(tmp_path, monkeypatch):
    import zipfile

    package = tmp_path / "package"
    package.mkdir()
    with zipfile.ZipFile(package / "VISR.ipa", "w") as exported:
        exported.writestr(
            "Payload/VISR.app/Info.plist",
            plistlib.dumps({
                "CFBundleIdentifier": "org.steverice.visr",
                "ITSAppUsesNonExemptEncryption": False,
            }),
        )
    entitlements = {
        "com.apple.developer.icloud-services": ["CloudKit"],
        "com.apple.developer.icloud-container-identifiers": [
            "iCloud.org.steverice.visr"
        ],
        "com.apple.developer.icloud-container-environment": "Production",
        "aps-environment": "development",
    }
    monkeypatch.setattr(
        validation, "run", lambda *args, **kwargs: plistlib.dumps(entitlements).decode()
    )
    with pytest.raises(ValueError, match="production APS"):
        validation.verify_exported_ipa(package)


@pytest.mark.parametrize("cloudkit_environment", ["Development", None])
def test_exported_ipa_rejects_nonproduction_cloudkit_environment(
    tmp_path, monkeypatch, cloudkit_environment
):
    import zipfile

    package = tmp_path / "package"
    package.mkdir()
    with zipfile.ZipFile(package / "VISR.ipa", "w") as exported:
        exported.writestr(
            "Payload/VISR.app/Info.plist",
            plistlib.dumps({
                "CFBundleIdentifier": "org.steverice.visr",
                "ITSAppUsesNonExemptEncryption": False,
            }),
        )
    entitlements = {
        "com.apple.developer.icloud-services": ["CloudKit"],
        "com.apple.developer.icloud-container-identifiers": [
            "iCloud.org.steverice.visr"
        ],
        "aps-environment": "production",
    }
    if cloudkit_environment is not None:
        entitlements["com.apple.developer.icloud-container-environment"] = (
            cloudkit_environment
        )
    monkeypatch.setattr(
        validation, "run", lambda *args, **kwargs: plistlib.dumps(entitlements).decode()
    )
    with pytest.raises(ValueError, match="production CloudKit environment"):
        validation.verify_exported_ipa(package)


@pytest.mark.parametrize("environment", ["Development", "Production", None])
def test_archive_cloudkit_allows_export_to_select_environment(environment):
    entitlements = {
        "com.apple.developer.icloud-services": ["CloudKit"],
        "com.apple.developer.icloud-container-identifiers": ["iCloud.org.steverice.visr"],
        "aps-environment": "production",
    }
    if environment is not None:
        entitlements["com.apple.developer.icloud-container-environment"] = environment
    validation.verify_benchmark_upload_entitlements(
        entitlements, "iCloud.org.steverice.visr", archive=True
    )


@pytest.mark.parametrize("field,value,error", [
    ("com.apple.developer.icloud-services", [], "CloudKit service"),
    ("com.apple.developer.icloud-container-identifiers", ["iCloud.example.wrong"], "CloudKit container"),
    ("com.apple.developer.icloud-container-environment", None, "CloudKit environment"),
    ("com.apple.developer.icloud-container-environment", "Other", "CloudKit environment"),
    ("com.apple.developer.icloud-container-environment", "", "CloudKit environment"),
    ("aps-environment", "development", "production APS"),
    ("aps-environment", None, "production APS"),
])
def test_archive_cloudkit_still_requires_capabilities_and_production_aps(field, value, error):
    entitlements = {
        "com.apple.developer.icloud-services": ["CloudKit"],
        "com.apple.developer.icloud-container-identifiers": ["iCloud.org.steverice.visr"],
        "com.apple.developer.icloud-container-environment": "Development",
        "aps-environment": "production",
    }
    entitlements[field] = value
    with pytest.raises(ValueError, match=error):
        validation.verify_benchmark_upload_entitlements(
            entitlements, "iCloud.org.steverice.visr", archive=True
        )


@pytest.mark.parametrize("declaration", [None, True, 0, "NO"])
def test_exported_ipa_rejects_missing_or_changed_encryption_exemption(
    tmp_path, monkeypatch, declaration
):
    import zipfile
    package = tmp_path / "package"
    package.mkdir()
    info = {} if declaration is None else {"ITSAppUsesNonExemptEncryption": declaration}
    with zipfile.ZipFile(package / "VISR.ipa", "w") as exported:
        exported.writestr("Payload/VISR.app/Info.plist", plistlib.dumps(info))

    def unexpected_codesign(*args, **kwargs):
        pytest.fail("Encryption policy must be checked before signing inspection")

    monkeypatch.setattr(validation, "signed_app_entitlements", unexpected_codesign)
    with pytest.raises(ValueError, match="encryption"):
        validation.verify_exported_ipa(package)


def optional_recording_policy() -> dict[str, str]:
    values = {**policy(), "recording_feed_required": "off"}
    del values["recording_feed_sha256"]
    return values


def test_policy_can_leave_the_recording_feed_unpinned_and_keeps_its_url() -> None:
    values = optional_recording_policy()
    loaded = validation.load_policy(json.dumps(values))
    assert loaded == values
    # The build still carries the URL, so cases published later reach installed apps.
    assert loaded["recording_feed_url"] == policy()["recording_feed_url"]
    assert not validation.recording_feed_required(loaded)
    assert validation.recording_feed_required(policy())
    with pytest.raises(ValueError, match="recording_feed_required"):
        validation.load_policy(json.dumps({**values, "recording_feed_required": "maybe"}))
    with pytest.raises(ValueError, match="exact documented"):
        validation.load_policy(
            json.dumps({**values, "recording_feed_sha256": "e" * 64})
        )
    required = {**values, "recording_feed_required": "on"}
    with pytest.raises(ValueError, match="exact documented"):
        validation.load_policy(json.dumps(required))


def test_preflight_without_a_pinned_recording_feed_skips_it_and_has_no_catalog_facts(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    values = optional_recording_policy()
    fetched: list[str] = []

    class Response:
        def __enter__(self) -> Self:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, _limit: int) -> bytes:
            return b"profile-envelope"

    class Opener:
        def open(self, url: str, timeout: int) -> Response:
            fetched.append(url)
            return Response()

    monkeypatch.setattr(
        validation.urllib.request, "build_opener", lambda *_args: Opener()
    )
    monkeypatch.setattr(validation, "verify_feed", lambda *_args, **_kwargs: None)
    assert validation.check_feeds(values) is None
    assert fetched == [values["profile_feed_url"]]
    assert validation.validate_recording_summary_context("", values) is None
    with pytest.raises(ValueError, match="without a pinned recording feed"):
        validation.validate_recording_summary_context('{"schema":1}', values)
    instructions = validation.release.release_summary_instructions(
        False, ["ios"], has_recording_catalog=False
    )
    assert "No current signed recording catalog facts were supplied" in instructions


def test_preflight_output_is_empty_without_catalog_facts(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    source = "a" * 40
    report = tmp_path / "report.json"
    report.write_text(json.dumps({"source_sha": source, "platforms": {"ios": {}}}))
    outputs: list[tuple[str, str]] = []
    monkeypatch.setattr(
        "sys.argv",
        ["release_validation.py", "check", "--source-sha", source,
         "--platforms", "ios", "--report", str(report)],
    )
    monkeypatch.setattr(validation.release, "require_ci", lambda: None)
    monkeypatch.setattr(validation, "check_release", lambda *_args: None)
    monkeypatch.setattr(validation, "preflight_identity", lambda *_args: "identity")
    monkeypatch.setattr(validation.release, "API", lambda *_args: object())
    monkeypatch.setattr(validation.release, "exact_app", lambda *_args: {})
    monkeypatch.setattr(
        validation.release, "output", lambda name, value: outputs.append((name, value))
    )
    monkeypatch.setenv("APPLE_BUNDLE_ID", "org.example.visr")
    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(optional_recording_policy()))
    validation.main()
    assert outputs == [("identity", "identity"), ("recording_context", "")]


def test_xcode_select_picks_an_accepted_build_and_exports_it(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    import plistlib

    from tools import xcode_builds, xcode_select

    def app(name: str, build: str) -> str:
        path = tmp_path / name / "Contents"
        path.mkdir(parents=True)
        (path / "version.plist").write_bytes(plistlib.dumps({"ProductBuildVersion": build}))
        return str(tmp_path / name)

    apps = [app("Xcode_27.app", "27A266a"), app("Xcode_27.2_beta_2.app", xcode_builds.XCODE_BUILDS[1])]
    env = tmp_path / "env"
    monkeypatch.setenv("GITHUB_ENV", str(env))
    monkeypatch.setattr(xcode_select.glob, "glob", lambda _pattern: apps)
    xcode_select.main()
    assert env.read_text() == f"DEVELOPER_DIR={apps[1]}/Contents/Developer\n"
    assert "27A266a" in capsys.readouterr().out
    monkeypatch.setattr(xcode_select.glob, "glob", lambda _pattern: apps[:1])
    with pytest.raises(SystemExit, match="None of the accepted Xcode builds"):
        xcode_select.main()


def test_release_workflow_selects_xcode_by_build_in_every_xcode_job() -> None:
    workflow = (validation.release.ROOT / ".github/workflows/release.yml").read_text()
    assert "DEVELOPER_DIR:" not in workflow
    assert workflow.count("run: python3 tools/xcode_select.py") == workflow.count(
        "runs-on: xcode-27"
    )


def test_cloud_saves_policy_is_explicit_and_defaults_off():
    assert not validation.cloud_saves_enabled(policy())
    enabled = {**policy(), "cloud_saves": "on"}
    assert validation.load_policy(json.dumps(enabled)) == enabled
    assert validation.cloud_saves_enabled(enabled)
    assert validation.load_policy(json.dumps({**policy(), "cloud_saves": "off"}))
    with pytest.raises(ValueError, match="cloud_saves"):
        validation.load_policy(json.dumps({**policy(), "cloud_saves": "yes"}))
    assert "--cloud-saves" not in validation.build_command("ios")
    command = validation.build_command("tvos", cloud_saves=True)
    assert "--cloud-saves" in command
    assert command[command.index("--cloud-container") + 1] == "iCloud.org.steverice.visr"
    with pytest.raises(ValueError, match="cloud-saves"):
        validation.compare_evidence({**policy(), "cloud_saves": "on"}, policy())
