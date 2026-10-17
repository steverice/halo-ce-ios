"""Recover with reviewed controller code while the application stays on its tag."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import sys

import pytest

from tools import release, release_validation
from tools.test_release import FakeAPI, build_page, localization
from tools.test_release_validation import policy


def git(root: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def repository(root: Path) -> None:
    root.mkdir()
    git(root, "init", "--quiet")
    git(root, "config", "user.name", "Release test")
    git(root, "config", "user.email", "release-test@example.invalid")
    (root / ".gitignore").write_text("build/\n")
    git(root, "add", ".gitignore")
    git(root, "commit", "--quiet", "--message", "fixture source")


def binaries() -> tuple[bytes, bytes]:
    guest = bytearray(264)
    guest[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", guest, 32, 64)
    struct.pack_into("<HH", guest, 54, 56, 2)
    for i, (flags, address, offset) in enumerate(
        [(5, 0x88000000, 256), (6, 0x88000004, 260)]
    ):
        struct.pack_into(
            "<IIQQQQQQ", guest, 64 + i * 56, 1, flags, offset, address, 0, 4, 4, 4
        )
    guest[256:] = b"CODEDATA"
    executable = bytearray(272)
    executable[:4] = b"\xcf\xfa\xed\xfe"
    struct.pack_into("<I", executable, 16, 1)
    struct.pack_into("<II", executable, 32, 0x19, 232)
    struct.pack_into("<I", executable, 96, 2)
    for i, (name, offset) in enumerate([(b"__halocode", 264), (b"__halodata", 268)]):
        start = 104 + i * 80
        executable[start : start + len(name)] = name
        struct.pack_into("<QI", executable, start + 40, 4, offset)
    executable[264:] = b"CODEDATA"
    executable.extend(hashlib.sha256(guest).hexdigest().encode())
    return bytes(guest), bytes(executable)


@pytest.fixture
def roots(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    application, controller = tmp_path / "application", tmp_path / "controller"
    for root in (application, controller):
        repository(root)
    for name, content in (
        ("pgo/halo_linux.profdata", b"guest profile"),
        ("pgo/visr_host.profdata", b"host profile"),
    ):
        path = application / name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(content)
    git(application, "add", "pgo")
    git(application, "commit", "--quiet", "--message", "fixture profiles")
    notes = "## 0.2.0\n\n- Preserve the reviewed release"
    manifest = {
        "tag": "visr-v0.2.0",
        "version": "0.2.0",
        "build_number": "20261017.34347",
        "source_sha": git(application, "rev-parse", "HEAD"),
        "platforms": ["ios"],
        "source_date_epoch": "1792211687",
        "notes_sha256": hashlib.sha256(notes.encode()).hexdigest(),
        "build_policy": policy(),
        "internal_only": True,
    }
    manifest["build_policy"].update(
        guest_sha256=hashlib.sha256(binaries()[0]).hexdigest(),
        guest_profile_sha256=hashlib.sha256(b"guest profile").hexdigest(),
        host_profile_sha256=hashlib.sha256(b"host profile").hexdigest(),
    )
    (application / ".release").mkdir()
    (application / release.MANIFEST).write_text(json.dumps(manifest))
    (application / "CHANGELOG.md").write_text(notes)
    git(application, "add", ".release/current.json", "CHANGELOG.md")
    git(application, "commit", "--quiet", "--message", "fixture release metadata")
    git(application, "tag", manifest["tag"])
    for root in (application, controller):
        git(root, "update-ref", "refs/remotes/origin/main", "HEAD")
    for name, value in {
        "GITHUB_ACTIONS": "true",
        "GITHUB_REF": "refs/heads/main",
        "GITHUB_EVENT_NAME": "workflow_dispatch",
        "GITHUB_SHA": git(controller, "rev-parse", "HEAD"),
        "GITHUB_REPOSITORY": "example/app",
        "GITHUB_WORKSPACE": str(application),
        "GH_TOKEN": "fixture",
    }.items():
        monkeypatch.setenv(name, value)
    monkeypatch.setattr(release, "ROOT", controller)
    return application, controller, manifest, notes


@pytest.mark.parametrize(
    "fault",
    [
        "source-dirty",
        "controller-dirty",
        "controller-sha",
        "source-parent",
        "source-tag",
        "nested-root",
        "overlap",
    ],
)
def test_publish_roots_reject_drift_before_api(
    roots, monkeypatch: pytest.MonkeyPatch, fault: str
):
    application, controller, manifest, _ = roots
    if fault.endswith("dirty"):
        (
            (application if fault.startswith("source") else controller)
            / "unexpected.txt"
        ).write_text("dirty")
    elif fault == "controller-sha":
        monkeypatch.setenv("GITHUB_SHA", "0" * 40)
    elif fault == "source-parent":
        manifest["source_sha"] = "1" * 40
        manifest["tag"] = "visr-v0.2.1"
        (application / release.MANIFEST).write_text(json.dumps(manifest))
        git(application, "add", ".release/current.json")
        git(application, "commit", "--quiet", "--message", "wrong parent fixture")
        git(application, "tag", "visr-v0.2.1")
    elif fault == "source-tag":
        git(
            application,
            "commit",
            "--allow-empty",
            "--quiet",
            "--message",
            "not the tag",
        )
    elif fault == "nested-root":
        application = application / ".release"
    else:
        application = controller
        monkeypatch.setenv("GITHUB_WORKSPACE", str(controller))
    monkeypatch.setattr(
        release, "API", lambda *_: pytest.fail("API reached before checkout guards")
    )
    expected = {
        "source-dirty": "must be clean",
        "controller-dirty": "must be clean",
        "controller-sha": "triggering workflow commit",
        "source-parent": "parent differs",
        "source-tag": "exact release tag",
        "nested-root": "original workspace",
        "overlap": "must be separate",
    }
    with pytest.raises(ValueError, match=expected[fault]):
        release.publish(argparse.Namespace(source_root=application))


@pytest.mark.parametrize("script_import", [False, True])
@pytest.mark.parametrize("corrupt_archive", [False, True])
def test_publish_reads_archive_after_xcode_consumes_normal_product(
    roots, monkeypatch: pytest.MonkeyPatch, script_import: bool, corrupt_archive: bool
):
    application, controller, manifest, notes = roots
    publisher, validation = release, release_validation
    if script_import:
        # Script execution imports top-level release_validation, which imports a
        # second release module. Its default ROOT must never select build inputs.
        monkeypatch.syspath_prepend(str(Path(release.__file__).parent))
        spec = importlib.util.spec_from_file_location(
            "release_controller_alias", release.__file__
        )
        publisher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(publisher)
        import release_validation as validation

        monkeypatch.setattr(publisher, "ROOT", controller)
    guest, executable = binaries()
    for name, value in {
        "APPLE_TEAM_ID": "fixture",
        "APPLE_BUNDLE_ID": "example.visr",
        "PROFILE_UUID_IOS": "profile",
        "ASC_KEY_PATH": "fixture.p8",
        "ASC_KEY_ID": "fixture",
        "ASC_ISSUER_ID": "fixture",
        "RELEASE_BUILD_POLICY": json.dumps(manifest["build_policy"]),
    }.items():
        monkeypatch.setenv(name, value)
    draft = {
        "id": "draft",
        "tag_name": manifest["tag"],
        "assets": [],
        "draft": True,
        "body": publisher.draft_body(notes, {}),
    }
    github = FakeAPI([[draft], {}, {}, {}, {}, {"tag_name": manifest["tag"], "body": notes, "draft": False, "assets": []}])
    apple = FakeAPI(
        [
            [{"id": "app-1", "attributes": {"bundleId": "example.visr"}}],
            {"data": []},
            build_page(
                version=manifest["version"],
                build=manifest["build_number"],
                state="PROCESSING",
            ),
            build_page(version=manifest["version"], build=manifest["build_number"]),
            [],
            {},
            [localization(notes)],
        ]
    )
    monkeypatch.setattr(
        publisher, "API", lambda base, _: github if "github" in base else apple
    )
    monkeypatch.setattr(publisher.time, "sleep", lambda _: None)
    monkeypatch.setattr(
        validation, "toolchain", lambda _, **kw: {"root": str(kw["root"])}
    )
    monkeypatch.setattr(
        validation,
        "check_feeds",
        lambda _, **kw: (
            None if kw["root"] == application else pytest.fail("feeds use controller")
        ),
    )
    commands = []
    original_run = publisher.run

    def run(*args, **kwargs):
        if args[0] == "git":
            return original_run(*args, **kwargs)
        assert kwargs["root"] == application
        commands.append(args)
        if "tools/ios_build.py" in args:
            normal = application / "build/ios/app-device/Release-iphoneos/VISR.app"
            product = application / "build/temporary-product/VISR.app"
            product.mkdir(parents=True)
            (application / "build/ios").mkdir(exist_ok=True)
            (application / "build/ios/halo_guest.elf").write_bytes(guest)
            info = {
                "CFBundleExecutable": "VISR",
                "CFBundleIdentifier": "example.visr",
                "CFBundleShortVersionString": manifest["version"],
                "CFBundleVersion": manifest["build_number"],
                "DTPlatformName": "iphoneos",
                "DTXcodeBuild": "27B5028f",
                "DTSDKName": "iphoneos27.2",
                "HaloDataPackPublicKey": validation.DATA_PACK_PUBLIC_KEY,
                "HaloProfileFeedURL": manifest["build_policy"]["profile_feed_url"],
                "HaloRecordingFeedURL": manifest["build_policy"]["recording_feed_url"],
                "HaloDataPackChannel": "tester",
            }
            archived_bytes = bytearray(executable)
            if corrupt_archive:
                archived_bytes[264] = 0
            (product / "Info.plist").write_bytes(plistlib.dumps(info))
            (product / "VISR").write_bytes(archived_bytes)
            normal.parent.mkdir(parents=True)
            normal.symlink_to(product)
            package = application / "build/release/ios"
            archive = package / "VISR.xcarchive/Products/Applications/VISR.app"
            archive.parent.mkdir(parents=True)
            product.rename(archive)
            (package / "ExportOptions.plist").write_bytes(
                plistlib.dumps({"testFlightInternalTestingOnly": True})
            )
            assert normal.is_symlink() and not normal.exists()
        return ""

    monkeypatch.setattr(publisher, "run", run)

    def inspect_command(*args, **kwargs):
        assert kwargs.get("root") == application
        if args[0] == "ninja":
            return "/llvm/clang -flto=full -fprofile-use=pgo/halo_linux.profdata -c source/game/player_control.c\npython tools/ios_guest_lto.py"
        if args[0] == "xcodebuild":
            return json.dumps(
                [
                    {
                        "target": "VISR",
                        "buildSettings": {
                            "OTHER_CFLAGS": "-fprofile-instr-use="
                            + str(application / "pgo/visr_host.profdata"),
                            "GCC_PREPROCESSOR_DEFINITIONS": "HALO_TESTER_BUILD=1 HALO_BENCHMARK_NO_ROUTES=1",
                        },
                    }
                ]
            )
        return manifest["build_policy"]["guest_compiler_version"]

    monkeypatch.setattr(validation, "run", inspect_command)
    if corrupt_archive:
        with pytest.raises(ValueError, match="Actual archived guest differs"):
            publisher.publish(argparse.Namespace(source_root=application))
        assert all(call[0] == "GET" for call in github.calls)
        assert not apple.calls
        assert all(command[0] != "xcodebuild" for command in commands)
        return
    publisher.publish(argparse.Namespace(source_root=application))
    assert len([c for c in commands if "tools/ios_build.py" in c]) == 1
    assert len([c for c in commands if c[0] == "xcodebuild"]) == 1
    checkpoints = [
        publisher.read_journal(c[2]["body"], notes)
        for c in github.calls
        if c[0] == "PATCH" and publisher.MARKER in c[2]["body"]
    ]
    assert [j["ios"]["state"] for j in checkpoints] == [
        "prepared",
        "uploading",
        "complete",
    ]
    assert github.calls[-2][2] == {"tag_name": manifest["tag"], "body": notes, "draft": False, "make_latest": "true"}
    assert any(
        call[0] == "POST" and call[1] == "/v1/betaBuildLocalizations"
        for call in apple.calls
    )
    assert not github.replies and not apple.replies
    assert git(application, "status", "--porcelain") == ""


def test_source_contract_subprocess_ignores_controller_module_roots(tmp_path: Path):
    source = tmp_path / "application"
    package = source / "tools"
    package.mkdir(parents=True)
    (package / "__init__.py").write_text("")
    (package / "release_validation.py").write_text(
        "from pathlib import Path\n"
        "def verify_feed(data, kind, policy):\n"
        " assert data == b'signed envelope' and kind == 'profile' and policy == {'fixture': 'policy'}\n"
        " (Path(__file__).parents[1] / 'verified-source.txt').write_text(str(Path(__file__).resolve()))\n"
    )
    release_validation.verify_feed(
        b"signed envelope", "profile", {"fixture": "policy"}, root=source
    )
    assert (source / "verified-source.txt").read_text() == str(
        package / "release_validation.py"
    )


def test_publish_cli_requires_explicit_source_root():
    result = subprocess.run(
        [sys.executable, str(Path(release.__file__)), "publish"],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2 and "--source-root" in result.stderr
