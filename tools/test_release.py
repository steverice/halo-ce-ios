"""Release identity, generated notes, recovery, and credential-free API contracts."""

from __future__ import annotations

import hashlib
import json
import re
import tomllib
import urllib.error
from pathlib import Path
from typing import Any

import pytest

from tools import release
from tools.test_release_validation import policy


class FakeAPI:
    def __init__(self, replies: list[Any]) -> None:
        self.replies = list(replies)
        self.calls: list[tuple[str, str, Any]] = []

    def request(self, method: str, path: str, body: Any = None) -> Any:
        self.calls.append((method, path, body))
        return self.replies.pop(0)

    def collection(self, path: str) -> list[dict[str, Any]]:
        return self.request("GET", path)


def build_page(
    platform: str = "IOS",
    version: str = "0.1.3",
    build: str = "20261016.010203",
    app: str = "app-1",
    state: str = "VALID",
) -> dict[str, Any]:
    return {
        "data": [
            {
                "type": "builds",
                "id": "build-1",
                "attributes": {
                    "version": build,
                    "processingState": state,
                    "buildAudienceType": "INTERNAL_ONLY",
                },
                "relationships": {
                    "app": {"data": {"id": app}},
                    "preReleaseVersion": {"data": {"id": "version-1"}},
                },
            }
        ],
        "included": [
            {
                "type": "preReleaseVersions",
                "id": "version-1",
                "attributes": {"version": version, "platform": platform},
            }
        ],
    }


def test_repository_changelog_parser_preserves_existing_gitmoji_and_types() -> None:
    config = tomllib.loads((release.ROOT / ".cz.toml").read_text())["tool"][
        "commitizen"
    ]
    assert config["version_provider"] == "scm"
    assert config["tag_format"] == "visr-v$version"
    parser = config["customize"]["commit_parser"]
    cases = {
        "⚡️ perf(metal): warm maps": "perf",
        "📦️ build(replays): bundle routes": "build",
        "🐛 fix(menus): dismiss dialog": "fix",
        "✨ feat(api)!: change interface": "feat",
        "⏪️ revert: restore behavior": "revert",
        "🔀 merge: integrate renderer": "merge",
        "feat: add touch input": "feat",
        "👷 ci: verify packages": "ci",
    }
    for subject, kind in cases.items():
        parsed = re.match(parser, subject)
        assert parsed and parsed.group("change_type") == kind
        assert re.match(config["customize"]["changelog_pattern"], subject)
    bump_map = config["customize"]["bump_map"]
    for subject, expected in [
        ("⚡️ perf(metal): warm maps", "PATCH"),
        ("feat: menu", "MINOR"),
        ("🐛 fix(api)!: rename", "MAJOR"),
    ]:
        token = re.search(config["customize"]["bump_pattern"], subject)
        assert token
        assert (
            next(
                value
                for key, value in bump_map.items()
                if re.match(key, token.group(1))
            )
            == expected
        )


@pytest.mark.parametrize("heading", ["0.2.0", "visr-v0.2.0"])
def test_notes_accept_commitizen_bump_heading_without_rewriting(heading: str) -> None:
    entry = f"## {heading} (2026-10-16)\n\n### fix\n\n- preserve generated notes"
    assert release.release_notes(entry + "\n\n## visr-v0.1.2\nold", "0.2.0") == entry
    for wrong in ("0.2.1", "0.2", "0.20.0"):
        with pytest.raises(ValueError, match="does not match"):
            release.release_notes(entry, wrong)


def test_notes_are_exact_generated_entry() -> None:
    entry = "## 0.1.3 (2026-10-16)\n\n### perf\n\n- warm maps"
    assert release.release_notes(entry + "\n\n## 0.1.2\nold", "0.1.3") == entry
    with pytest.raises(ValueError, match="does not match"):
        release.release_notes(entry, "0.2.0")
    with pytest.raises(ValueError, match="4000"):
        release.release_notes("## 0.1.3\n" + "x" * 4000, "0.1.3")


@pytest.mark.parametrize("value", ["mac", "ios,ios", "ios,", "ios, tvos", ""])
def test_platforms_reject_unsupported_or_ambiguous_targets(value: str) -> None:
    with pytest.raises(ValueError):
        release.platforms(value)


@pytest.mark.parametrize(
    "replacement",
    [
        {"platform": "TV_OS"},
        {"version": "0.1.2"},
        {"build": "20261016.999999"},
        {"app": "other-app"},
    ],
)
def test_build_lookup_checks_all_identity_fields(replacement: dict[str, str]) -> None:
    api = FakeAPI([build_page(**replacement)])
    assert release.exact_build(api, "app-1", "IOS", "0.1.3", "20261016.010203") is None
    query = api.calls[0][1]
    assert "filter%5Bapp%5D=app-1" in query
    assert "filter%5BpreReleaseVersion.platform%5D=IOS" in query
    assert "sort=" not in query


def test_processing_poll_waits_for_exact_build_without_sleep(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    api = FakeAPI([{"data": []}, build_page(state="PROCESSING"), build_page()])
    monkeypatch.setattr(release.time, "sleep", lambda _: None)
    assert (
        release.wait_for_build(
            api, "app-1", "IOS", "0.1.3", "20261016.010203", attempts=3
        )["id"]
        == "build-1"
    )


@pytest.mark.parametrize("state", ["FAILED", "INVALID"])
def test_failed_processing_does_not_report_success(state: str) -> None:
    with pytest.raises(ValueError, match=state):
        release.wait_for_build(
            FakeAPI([build_page(state=state)]),
            "app-1",
            "IOS",
            "0.1.3",
            "20261016.010203",
            attempts=1,
        )


def test_absent_build_times_out_without_creating_another_build() -> None:
    api = FakeAPI([{"data": []}])
    with pytest.raises(TimeoutError, match="resume this tag"):
        release.wait_for_build(
            api, "app-1", "IOS", "0.1.3", "20261016.010203", attempts=1
        )
    assert all(call[0] == "GET" for call in api.calls)


def test_external_eligible_build_is_rejected() -> None:
    page = build_page()
    page["data"][0]["attributes"]["buildAudienceType"] = "APP_STORE_ELIGIBLE"
    with pytest.raises(ValueError, match="internal"):
        release.wait_for_build(
            FakeAPI([page]), "app-1", "IOS", "0.1.3", "20261016.010203", attempts=1
        )


def localization(notes: str | None, locale: str = "en-US") -> dict[str, Any]:
    return {"id": locale, "attributes": {"locale": locale, "whatsNew": notes}}


def test_notes_create_preserves_other_locales_and_verifies_exact_text() -> None:
    notes = "## 0.1.3\n\n- Fix input"
    french = localization("Texte existant", "fr-FR")
    api = FakeAPI([[french], {}, [french, localization(notes)]])
    release.set_testflight_notes(api, "build-1", notes)
    request = api.calls[1]
    assert request[:2] == ("POST", "/v1/betaBuildLocalizations")
    assert request[2]["data"]["attributes"] == {"locale": "en-US", "whatsNew": notes}
    assert request[2]["data"]["relationships"]["build"]["data"]["id"] == "build-1"


def test_notes_resume_is_idempotent_and_preserves_human_text() -> None:
    same = FakeAPI([[localization("notes")]])
    release.set_testflight_notes(same, "build-1", "notes")
    assert len(same.calls) == 1
    other = FakeAPI([[localization("human edits")]])
    with pytest.raises(ValueError, match="human text"):
        release.set_testflight_notes(other, "build-1", "notes")
    assert len(other.calls) == 1


def test_notes_patch_only_empty_selected_locale() -> None:
    api = FakeAPI([[localization(None)], {}, [localization("notes")]])
    release.set_testflight_notes(api, "build-1", "notes")
    assert api.calls[1][0] == "PATCH"


def test_notes_write_that_does_not_stick_fails() -> None:
    with pytest.raises(ValueError, match="retain"):
        release.set_testflight_notes(FakeAPI([[], {}, []]), "build-1", "notes")


def test_draft_journal_survives_partial_upload_and_detects_edited_notes() -> None:
    journal = {"ios": {"state": "complete"}, "tvos": {"state": "uploading"}}
    body = release.draft_body("notes", journal)
    assert release.read_journal(body, "notes") == journal
    with pytest.raises(ValueError):
        release.read_journal(body, "different notes")


def test_draft_release_lookup_includes_authenticated_pages() -> None:
    draft = {"tag_name": "visr-v0.1.3", "draft": True}
    api = FakeAPI([[{"tag_name": f"other-{n}"} for n in range(100)], [draft]])
    assert (
        release.find_release(api, "/repos/example/app/releases", draft["tag_name"])
        == draft
    )
    assert api.calls[-1][1].endswith("page=2")


def test_mutating_http_error_is_never_blindly_retried(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    count = 0

    def fail(*_args: Any, **_kwargs: Any) -> None:
        nonlocal count
        count += 1
        raise urllib.error.HTTPError(
            "https://example.invalid", 503, "Unavailable", {}, None
        )

    monkeypatch.setattr(release.urllib.request, "urlopen", fail)
    with pytest.raises(urllib.error.HTTPError):
        release.API("https://example.invalid", "dummy").request("POST", "/release", {})
    assert count == 1


def test_mutations_require_ci_main(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    with pytest.raises(ValueError, match="workflow_dispatch"):
        release.require_ci()


def test_published_release_resume_does_not_rebuild_or_republish(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    notes = "## 0.1.3\n\n- Fix input"
    manifest = {
        "tag": "visr-v0.1.3",
        "version": "0.1.3",
        "notes_sha256": hashlib.sha256(notes.encode()).hexdigest(),
    }
    (tmp_path / ".release").mkdir()
    (tmp_path / release.MANIFEST).write_text(json.dumps(manifest))
    (tmp_path / "CHANGELOG.md").write_text(notes)
    github = FakeAPI(
        [[{"tag_name": manifest["tag"], "assets": [], "draft": False, "body": notes}]]
    )
    monkeypatch.setattr(release, "ROOT", tmp_path)
    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "publish_context", lambda _: (tmp_path, manifest))
    monkeypatch.setattr(release, "run", lambda *_args, **_kwargs: "same-commit")
    monkeypatch.setattr(release, "API", lambda base, token: github)
    monkeypatch.setenv("GH_TOKEN", "dummy")
    monkeypatch.setenv("GITHUB_REPOSITORY", "example/app")
    release.publish(type("Args", (), {"source_root": tmp_path})())
    assert len(github.calls) == 1


def test_prepare_bootstrap_generates_one_version_and_atomic_push(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(release.time, "strftime", lambda *_: "20261016.000000")
    source = "a" * 40
    baseline = "b" * 40
    calls: list[tuple[Any, ...]] = []
    notes = "## visr-v0.1.3\n\n### perf\n\n- Warm maps"

    def fake_run(*args: Any, **kwargs: Any) -> str:
        calls.append(args)
        if args[:3] == ("git", "rev-parse", "origin/main"):
            return source
        if args[:3] == ("cz", "bump", "--get-next"):
            return "0.1.3"
        if args[:2] == ("cz", "bump"):
            (tmp_path / "CHANGELOG.md").write_text(notes)
        if args[:3] == ("git", "show", "--no-patch"):
            return "1760576523"
        return ""

    monkeypatch.setattr(release, "ROOT", tmp_path)
    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "run", fake_run)
    monkeypatch.setattr(release, "output", lambda *_args: None)
    args = type(
        "Args",
        (),
        {
            "source_sha": source,
            "platforms": "ios,tvos",
            "resume_tag": "",
            "bootstrap_since": baseline,
            "bootstrap_version": "0.1.2",
            "increment": "patch",
        },
    )()
    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(policy()))
    from tools.release_validation import preflight_identity

    monkeypatch.setenv(
        "RELEASE_PREFLIGHT_IDENTITY",
        preflight_identity(source, ["ios", "tvos"], policy()),
    )
    release.prepare(args)
    manifest = json.loads((tmp_path / release.MANIFEST).read_text())
    assert manifest["version"] == "0.1.3"
    assert manifest["build_number"] == "20261016.0"
    assert manifest["source_sha"] == source
    assert manifest["bootstrap_since"] == baseline
    assert manifest["notes_sha256"] == hashlib.sha256(notes.encode()).hexdigest()
    pushes = [call for call in calls if call[:2] == ("git", "push")]
    assert pushes == [
        (
            "git",
            "push",
            "--atomic",
            "origin",
            "HEAD:refs/heads/main",
            "refs/tags/visr-v0.1.3",
        )
    ]
    assert ("git", "tag", "visr-v0.1.2", baseline) in calls
    assert (
        "cz",
        "bump",
        "--yes",
        "--files-only",
        "--changelog",
        "--increment",
        "PATCH",
    ) in calls


def test_prepare_refuses_stale_main_before_any_mutation(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls: list[Any] = []

    def fake_run(*args: Any, **kwargs: Any) -> str:
        calls.append(args)
        return "b" * 40

    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "run", fake_run)
    args = type(
        "Args", (), {"source_sha": "a" * 40, "platforms": "ios", "resume_tag": ""}
    )()
    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(policy()))
    from tools.release_validation import preflight_identity

    monkeypatch.setenv(
        "RELEASE_PREFLIGHT_IDENTITY", preflight_identity("a" * 40, ["ios"], policy())
    )
    with pytest.raises(ValueError, match="Main moved"):
        release.prepare(args)
    assert not any(
        call[:2] in [("cz", "bump"), ("git", "push"), ("git", "commit")]
        for call in calls
    )


def test_cleanup_removes_secret_files_even_if_search_restore_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    import subprocess

    from tools import release_signing

    secret = tmp_path / "key.p8"
    secret.write_text("fixture key")
    (tmp_path / "cleanup.json").write_text(
        json.dumps(
            {
                "search": ["old"],
                "keychain": str(tmp_path / "absent"),
                "profiles": [],
                "files": [str(secret)],
            }
        )
    )

    def fail(*_args: Any, **_kwargs: Any) -> str:
        raise subprocess.CalledProcessError(1, "security")

    monkeypatch.setattr(release_signing, "security", fail)
    with pytest.raises(OSError, match="cleanup"):
        release_signing.cleanup(tmp_path)
    assert not secret.exists()
    assert (tmp_path / "cleanup.json").exists()


def test_partial_release_resume_polls_uploading_platform_without_build_or_upload(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    notes = "## 0.1.3\n\n- Fix input"
    manifest = {
        "build_policy": policy(),
        "tag": "visr-v0.1.3",
        "version": "0.1.3",
        "build_number": "20261016.010203",
        "platforms": ["ios", "tvos"],
        "source_date_epoch": "1760576523",
        "notes_sha256": hashlib.sha256(notes.encode()).hexdigest(),
    }
    (tmp_path / ".release").mkdir()
    (tmp_path / release.MANIFEST).write_text(json.dumps(manifest))
    (tmp_path / "CHANGELOG.md").write_text(notes)
    journal = {
        platform: {"state": state, "app_id": "app-1", "guest_sha256": "guest"}
        for platform, state in [("ios", "complete"), ("tvos", "uploading")]
    }
    draft = {
        "id": "release-1",
        "tag_name": manifest["tag"],
        "assets": [],
        "draft": True,
        "body": release.draft_body(notes, journal),
    }
    github = FakeAPI([[draft], {}, {}, {}, {"tag_name": manifest["tag"], "body": notes, "draft": False, "assets": []}])
    apple = FakeAPI(
        [
            build_page(),
            build_page(),
            [localization(notes)],
            {"data": []},
            build_page(platform="TV_OS"),
            [localization(notes)],
        ]
    )
    commands: list[Any] = []

    def fake_run(*args: Any, **kwargs: Any) -> str:
        commands.append(args)
        return "same-commit"

    monkeypatch.setattr(release, "ROOT", tmp_path)
    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "run", fake_run)
    monkeypatch.setattr(
        release, "API", lambda base, token: github if "github" in base else apple
    )
    monkeypatch.setenv("GH_TOKEN", "dummy")
    monkeypatch.setenv("GITHUB_REPOSITORY", "example/app")
    from tools import release_validation

    monkeypatch.setattr(release, "publish_context", lambda _: (tmp_path, manifest))
    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(policy()))
    monkeypatch.setattr(release_validation, "toolchain", lambda *_, **__: {})
    monkeypatch.setattr(release_validation, "check_feeds", lambda *_, **__: None)
    release.publish(type("Args", (), {"source_root": tmp_path})())
    assert all(command[:2] == ("git", "rev-parse") for command in commands)
    assert all(call[0] == "GET" for call in apple.calls)
    assert github.calls[-2][2] == {"tag_name": manifest["tag"], "body": notes, "draft": False, "make_latest": "true"}
    assert github.calls[-1][:2] == ("GET", "/repos/example/app/releases/release-1")


def test_verify_guest_checks_real_macho_section_bytes() -> None:
    import struct

    guest = bytearray(264)
    guest[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", guest, 32, 64)
    struct.pack_into("<HH", guest, 54, 56, 2)
    for index, (flags, address, offset) in enumerate(
        [(5, 0x88000000, 256), (6, 0x88000004, 260)]
    ):
        struct.pack_into(
            "<IIQQQQQQ", guest, 64 + index * 56, 1, flags, offset, address, 0, 4, 4, 4
        )
    guest[256:] = b"CODEDATA"
    digest = hashlib.sha256(guest).hexdigest()
    executable = bytearray(272)
    executable[:4] = b"\xcf\xfa\xed\xfe"
    struct.pack_into("<I", executable, 16, 1)
    struct.pack_into("<II", executable, 32, 0x19, 232)
    struct.pack_into("<I", executable, 96, 2)
    for index, (name, offset) in enumerate(
        [(b"__halocode", 264), (b"__halodata", 268)]
    ):
        section = 104 + index * 80
        executable[section : section + len(name)] = name
        struct.pack_into("<QI", executable, section + 40, 4, offset)
    executable[264:] = b"CODEDATA"
    executable.extend(digest.encode())
    release.verify_guest(bytes(executable), bytes(guest), digest)
    executable[264] = 0
    with pytest.raises(ValueError, match="Actual archived guest differs"):
        release.verify_guest(bytes(executable), bytes(guest), digest)


def test_signing_installs_only_selected_existing_material_and_cleans_it(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    import base64
    import datetime
    import plistlib

    from tools import release_signing

    home = tmp_path / "home"
    monkeypatch.setattr(Path, "home", lambda: home)
    certificate = b"fixture distribution certificate"
    profile = {
        "TeamIdentifier": ["TESTTEAM"],
        "Platform": ["iOS"],
        "UUID": "ABCDEF-1234",
        "Entitlements": {
            "application-identifier": "TESTTEAM.example.app",
            "get-task-allow": False,
            "com.apple.developer.kernel.extended-virtual-addressing": True,
        },
        "ExpirationDate": datetime.datetime.now(datetime.UTC).replace(tzinfo=None)
        + datetime.timedelta(days=1),
        "DeveloperCertificates": [certificate],
    }
    commands: list[tuple[str, ...]] = []

    def security(*args: str, capture: bool = False) -> str:
        commands.append(args)
        if args[:2] == ("list-keychains", "-d") and "-s" not in args:
            return '"original.keychain-db"'
        if args[0] == "create-keychain":
            Path(args[-1]).touch()
        if args[0] == "delete-keychain":
            Path(args[-1]).unlink()
        if args[0] == "find-identity":
            return hashlib.sha1(certificate).hexdigest().upper()
        if args[0] == "cms":
            return plistlib.dumps(profile).decode()
        return ""

    monkeypatch.setattr(release_signing, "security", security)
    for key, value in {
        "APPLE_TEAM_ID": "TESTTEAM",
        "APPLE_BUNDLE_ID": "example.app",
        "APPLE_DISTRIBUTION_P12_BASE64": base64.b64encode(b"fixture-p12").decode(),
        "APPLE_DISTRIBUTION_P12_PASSWORD": "fixture-password",
        "RELEASE_PLATFORMS": "ios",
        "APPLE_PROFILE_IOS_BASE64": base64.b64encode(b"fixture-profile").decode(),
        "GITHUB_ENV": str(tmp_path / "github-env"),
        "ASC_PRIVATE_KEY": "fixture-private-key",
    }.items():
        monkeypatch.setenv(key, value)
    folder = tmp_path / "credentials"
    release_signing.install(folder)
    env = (tmp_path / "github-env").read_text()
    assert "PROFILE_UUID_IOS=ABCDEF-1234" in env
    assert "PROFILE_UUID_TVOS" not in env
    assert (folder / "AuthKey.p8").stat().st_mode & 0o777 == 0o600
    assert not (folder / "distribution.p12").exists()
    release_signing.cleanup(folder)
    assert not (folder / "AuthKey.p8").exists()
    assert not list(home.rglob("*.mobileprovision"))
    assert ("list-keychains", "-d", "user", "-s", "original.keychain-db") in commands


def test_prepare_rejects_unvalidated_policy_before_git_or_version_mutation(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(policy()))
    monkeypatch.delenv("RELEASE_PREFLIGHT_IDENTITY", raising=False)
    calls = []
    monkeypatch.setattr(release, "run", lambda *args, **kwargs: calls.append(args))
    args = type("Args", (), {"source_sha": "a" * 40, "platforms": "ios,tvos"})()
    with pytest.raises(ValueError, match="after release preflight"):
        release.prepare(args)
    assert calls == []


def test_exact_app_ignores_prefix_matches_and_consumes_all_pages(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    exact = {"id": "exact-app", "attributes": {"bundleId": "org.example.visr"}}
    prefix = {"id": "prefix-app", "attributes": {"bundleId": "org.example.visr.tvos"}}
    api = release.API("https://api.appstoreconnect.apple.com", "fixture")
    responses = FakeAPI(
        [
            {
                "data": [exact, prefix],
                "links": {"next": api.base + "/v1/apps?cursor=next"},
            },
            {
                "data": [
                    {
                        "id": "another-app",
                        "attributes": {"bundleId": "org.example.other"},
                    }
                ]
            },
        ]
    )
    monkeypatch.setattr(api, "request", responses.request)
    assert release.exact_app(api, "org.example.visr") == exact
    assert len(responses.calls) == 2
    assert "filter%5BbundleId%5D=org.example.visr" in responses.calls[0][1]
    assert responses.calls[1][1] == "/v1/apps?cursor=next"


@pytest.mark.parametrize(
    "later", [[], [{"id": "duplicate", "attributes": {"bundleId": "org.example.visr"}}]]
)
def test_exact_app_rejects_missing_or_duplicate_exact_match(
    monkeypatch: pytest.MonkeyPatch, later: list[Any]
) -> None:
    api = release.API("https://api.appstoreconnect.apple.com", "fixture")
    initial = {
        "id": "first",
        "attributes": {
            "bundleId": "org.example.visr" if later else "org.example.visr.tvos"
        },
    }
    responses = FakeAPI(
        [
            {"data": [initial], "links": {"next": api.base + "/v1/apps?cursor=next"}},
            {"data": later},
        ]
    )
    monkeypatch.setattr(api, "request", responses.request)
    with pytest.raises(
        ValueError, match="exactly one existing App Store Connect record"
    ):
        release.exact_app(api, "org.example.visr")
    assert len(responses.calls) == 2


def test_exact_app_refuses_pagination_to_another_origin(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    api = release.API("https://api.appstoreconnect.apple.com", "fixture")
    responses = FakeAPI(
        [{"data": [], "links": {"next": "https://example.invalid/steal-token"}}]
    )
    monkeypatch.setattr(api, "request", responses.request)
    with pytest.raises(ValueError, match="pagination host"):
        release.exact_app(api, "org.example.visr")
    assert len(responses.calls) == 1


@pytest.mark.parametrize(
    ("padded", "canonical"),
    [
        ("20261016.011357", "20261016.11357"),
        ("20261016.005518", "20261016.5518"),
        ("20261016.000000", "20261016.0"),
        ("0001.00.001", "1.0.1"),
    ],
)
def test_build_number_canonicalizes_only_numeric_component_padding(
    padded: str, canonical: str
) -> None:
    assert release.canonical_build_number(padded) == canonical
    assert release.canonical_build_number(canonical) == canonical


@pytest.mark.parametrize(
    ("requested", "actual"),
    [
        ("20261016.011357", "20261016.11357"),
        ("20261016.005518", "20261016.5518"),
        ("20261016.000000", "20261016.0"),
        ("20261016.11357", "20261016.011357"),
    ],
)
def test_exact_build_recovers_padded_manifest_against_apple_canonical_number(
    requested: str, actual: str
) -> None:
    api = FakeAPI([build_page(build=actual)])
    assert (
        release.exact_build(api, "app-1", "IOS", "0.1.3", requested)["id"] == "build-1"
    )
    query = api.calls[0][1]
    expected = "20261016." + str(int(requested.split(".")[1]))
    assert "filter%5Bversion%5D=" + expected + "&" in query


def test_canonical_build_number_does_not_match_a_distinct_build() -> None:
    api = FakeAPI([build_page(build="20261016.11358")])
    assert release.exact_build(api, "app-1", "IOS", "0.1.3", "20261016.011357") is None


@pytest.mark.parametrize("value", ["", "1..2", "-1.2", "1.2a", "1.2.3.4"])
def test_canonical_build_number_rejects_non_numeric_components(value: str) -> None:
    with pytest.raises(ValueError, match="numeric components"):
        release.canonical_build_number(value)


@pytest.mark.parametrize("platform", ["ios", "tvos", "visionos"])
def test_release_profiles_require_the_archive_extended_addressing_entitlement(
    platform: str,
) -> None:
    from tools import release_signing

    key = "com.apple.developer.kernel.extended-virtual-addressing"
    profile = {
        "Entitlements": {
            key: True,
            "com.apple.developer.kernel.increased-memory-limit": True,
        }
    }
    release_signing.validate_profile_entitlements(profile, platform)
    for missing in [None, False, 1, "true"]:
        profile["Entitlements"][key] = missing
        with pytest.raises(ValueError, match="extended-virtual-addressing"):
            release_signing.validate_profile_entitlements(profile, platform)


def test_vision_release_profile_requires_its_existing_increased_memory_entitlement() -> (
    None
):
    from tools import release_signing

    profile = {
        "Entitlements": {"com.apple.developer.kernel.extended-virtual-addressing": True}
    }
    with pytest.raises(ValueError, match="increased-memory-limit"):
        release_signing.validate_profile_entitlements(profile, "visionos")


def test_benchmark_upload_release_argument_defaults_off_and_is_policy_pinned():
    assert release.benchmark_upload_arguments(policy()) == []
    assert release.benchmark_upload_arguments({**policy(), "benchmark_upload": "off"}) == []
    assert release.benchmark_upload_arguments({**policy(), "benchmark_upload": "on"}) == [
        "--benchmark-upload"
    ]
