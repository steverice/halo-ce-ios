"""Release identity, generated notes, recovery, and credential-free API contracts."""

from __future__ import annotations

import base64
import hashlib
import io
import json
import re
import tomllib
import urllib.error
from pathlib import Path
from typing import Any, Self

import pytest

from tools import release
from tools.test_release_assets import asset_policy as policy


def recording_summary_context() -> dict[str, Any]:
    current_policy = policy()
    guest = current_policy["guest_sha256"]
    return {
        "schema": 1,
        "guest_sha256": guest,
        "envelope_sha256": current_policy["recording_feed_sha256"],
        "payload_sha256": "f" * 64,
        "catalog_sha256": "1" * 64,
        "compatibility": "benchmark-catalog-1:" + guest,
        "cases": [
            {
                "id": "mp-chillout-proof",
                "kind": "multiplayer",
                "map": "chillout",
                "players": 16,
            },
            {
                "id": "mp-bloodgulch-proof",
                "kind": "multiplayer",
                "map": "bloodgulch",
                "players": 32,
            },
        ],
    }


def _configure_wif_environment(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv(
        "ACTIONS_ID_TOKEN_REQUEST_URL",
        "https://pipelines.actions.githubusercontent.com/test-id?api-version=2",
    )
    monkeypatch.setenv("ACTIONS_ID_TOKEN_REQUEST_TOKEN", "github-request-token-fixture")
    monkeypatch.setenv("ANTHROPIC_FEDERATION_RULE_ID", "fdrl_test")
    monkeypatch.setenv(
        "ANTHROPIC_ORGANIZATION_ID", "00000000-0000-0000-0000-000000000000"
    )
    monkeypatch.setenv("ANTHROPIC_SERVICE_ACCOUNT_ID", "svac_test")
    monkeypatch.setenv("ANTHROPIC_WORKSPACE_ID", "wrkspc_test")
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    monkeypatch.setenv("GITHUB_EVENT_NAME", "workflow_dispatch")
    monkeypatch.setenv("GITHUB_REF", "refs/heads/main")
    monkeypatch.setenv("GITHUB_REPOSITORY", "steverice/visr")
    monkeypatch.setenv(
        "GITHUB_WORKFLOW_REF",
        "steverice/visr/.github/workflows/release.yml@refs/heads/main",
    )
    monkeypatch.setenv("GITHUB_JOB", "prepare")


def _github_oidc_jwt_fixture(**changes: Any) -> str:
    claims: dict[str, Any] = {
        "iss": "https://token.actions.githubusercontent.com",
        "aud": "https://api.anthropic.com",
        "sub": "repo:steverice@1423851/visr@1408152155:environment:release",
        "repository": "steverice/visr",
        "repository_owner": "steverice",
        "ref": "refs/heads/main",
        "event_name": "workflow_dispatch",
        "workflow_ref": "steverice/visr/.github/workflows/release.yml@refs/heads/main",
        "environment": "release",
    }
    claims.update(changes)
    payload = (
        base64.urlsafe_b64encode(json.dumps(claims).encode()).rstrip(b"=").decode()
    )
    return f"header.{payload}.signature"


@pytest.fixture(autouse=True)
def _release_wif_test_environment(monkeypatch: pytest.MonkeyPatch) -> None:
    _configure_wif_environment(monkeypatch)


def _stub_wif_access_token(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(
        release,
        "exchange_anthropic_access_token",
        lambda **_kwargs: (
            "sk-ant-oat01-test-access-token",
            {
                "scope": "workspace:inference",
                "expires_in": 600,
            },
        ),
    )


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


def test_full_release_entry_can_exceed_testflight_note_limit() -> None:
    entry = "## 0.3.0\n\n### Full changelog\n\n" + "x" * 8_500
    assert release.release_notes(entry, "0.3.0") == entry


def test_testflight_summary_is_inserted_before_unchanged_details() -> None:
    details = "### feat\n\n- Add bigger multiplayer battles.\n\n### fix\n\n- Keep recorded animation settings."
    entry = "## 0.3.0 (2026-10-18)\n\n" + details
    summary = "- Play larger multiplayer battles.\n- Replays now preserve recorded animation settings."

    inserted = release.insert_testflight_summary(entry, summary)

    assert inserted == (
        "## 0.3.0 (2026-10-18)\n\n"
        "### Summary\n\n"
        "- Play larger multiplayer battles.\n- Replays now preserve recorded animation settings.\n\n"
        "### Full changelog\n\n" + details
    )
    assert release.extract_testflight_summary(inserted) == summary


def test_changelog_summary_insertion_preserves_older_entries() -> None:
    summary = "Try larger multiplayer battles."
    changelog = (
        "## 0.3.0 (2026-10-18)\n\n### feat\n\n- Add multiplayer cases.\n\n"
        "## 0.2.1 (2026-10-17)\n\n### fix\n\n- Keep the older release text.\n"
    )

    inserted = release.insert_testflight_summary_into_changelog(
        changelog, "0.3.0", summary
    )

    assert inserted.split("\n\n## 0.2.1", 1)[1] == changelog.split("\n\n## 0.2.1", 1)[1]
    assert release.release_notes(
        inserted, "0.3.0"
    ) == release.insert_testflight_summary(
        "## 0.3.0 (2026-10-18)\n\n### feat\n\n- Add multiplayer cases.", summary
    )


@pytest.mark.parametrize(
    "value",
    ["", " \n ", b"\xff", "x" * 4_001, "summary\n### Full changelog\nforged", 42],
)
def test_testflight_summary_rejects_invalid_output(value: str | bytes | int) -> None:
    error = TypeError if isinstance(value, int) else ValueError
    with pytest.raises(error):
        release.validate_testflight_summary(value)


def test_summary_insertion_refuses_to_overwrite_an_existing_summary() -> None:
    entry = "## 0.3.0\n\n### Summary\n\nExisting human text\n\n### Full changelog\n\n### feat\n\n- A change"
    with pytest.raises(ValueError, match="already contains"):
        release.insert_testflight_summary(entry, "A replacement summary")
    with pytest.raises(ValueError, match="Markdown headings"):
        release.insert_testflight_summary(
            "## 0.3.0\n\n### feat\n\n- A change", "  # Hidden heading"
        )


def test_new_manifest_pins_summary_and_rejects_hash_or_content_mismatch() -> None:
    details = "### feat\n\n- Add a multiplayer case."
    summary = "Play a new multiplayer case."
    notes = release.release_notes(
        release.insert_testflight_summary("## 0.3.0\n\n" + details, summary),
        "0.3.0",
    )
    manifest = {
        "notes_sha256": hashlib.sha256(notes.encode()).hexdigest(),
        "testflight_summary": summary,
        "testflight_summary_sha256": hashlib.sha256(summary.encode()).hexdigest(),
        "testflight_summary_model": "claude-sonnet-5-5",
    }

    assert release.testflight_summary_for_release(notes, manifest) == summary
    with pytest.raises(ValueError, match="summary hash"):
        release.testflight_summary_for_release(
            notes, {**manifest, "testflight_summary_sha256": "0" * 64}
        )
    changed = notes.replace("Play a new multiplayer case.", "Unverified new feature.")
    with pytest.raises(ValueError, match="summary differs"):
        release.testflight_summary_for_release(changed, manifest)
    indented_heading = "  ## Not a summary\n\n" + summary
    with pytest.raises(ValueError, match="Markdown heading"):
        release.testflight_summary_for_release(
            notes,
            {
                **manifest,
                "testflight_summary": indented_heading,
                "testflight_summary_sha256": hashlib.sha256(
                    indented_heading.encode()
                ).hexdigest(),
            },
        )


def test_legacy_manifest_reuses_full_note_only_within_testflight_limit() -> None:
    notes = "## 0.2.1\n\n### fix\n\n- Preserve the existing TestFlight note."
    assert release.testflight_summary_for_release(notes, {"version": "0.2.1"}) == notes
    with pytest.raises(ValueError, match="Legacy TestFlight note exceeds"):
        release.testflight_summary_for_release("## 0.2.1\n" + "x" * 4_001, {})


def test_anthropic_summary_uses_messages_api_and_release_facts(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    summary = "Play larger multiplayer battles."
    body = json.dumps(
        {
            "type": "message",
            "role": "assistant",
            "stop_reason": "end_turn",
            "content": [
                {"type": "thinking", "thinking": "private reasoning content"},
                {"type": "redacted_thinking", "data": "redacted block"},
                {"type": "text", "text": summary},
            ],
        }
    ).encode()
    calls: list[tuple[Any, int]] = []

    class FakeResponse:
        def __enter__(self) -> Self:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, _limit: int = -1) -> bytes:
            return body

    class FakeOpener:
        def open(self, request: Any, timeout: int) -> FakeResponse:
            calls.append((request, timeout))
            return FakeResponse()

    _stub_wif_access_token(monkeypatch)
    monkeypatch.setenv("ANTHROPIC_API_KEY", "legacy-key-must-not-win")
    monkeypatch.setenv("RELEASE_SUMMARY_MODEL", "claude-sonnet-5-5")
    monkeypatch.setattr(
        release.urllib.request, "build_opener", lambda *_handlers: FakeOpener()
    )

    result = release.anthropic_release_summary(
        "## 0.3.0\n\n### feat\n\n- The early multiplayer prototype supported two and four players.",
        version="0.3.0",
        selected_platforms=["ios", "tvos"],
        benchmark_upload=False,
        catalog_context=recording_summary_context(),
    )

    assert result == summary
    assert len(calls) == 1
    request, timeout = calls[0]
    assert request.full_url == "https://api.anthropic.com/v1/messages"
    assert (
        request.get_header("Authorization") == "Bearer sk-ant-oat01-test-access-token"
    )
    assert request.get_header("X-api-key") is None
    assert request.get_header("Anthropic-version") == "2023-06-01"
    assert timeout == 60
    payload = json.loads(request.data)
    assert payload["model"] == "claude-sonnet-5-5"
    assert payload["max_tokens"] == 4_096
    assert payload["thinking"] == {"type": "adaptive"}
    assert payload["output_config"] == {"effort": "low"}
    assert "American English" in payload["system"]
    assert "untrusted" in payload["system"].lower()
    assert "newest-first" in payload["system"]
    assert (
        "current benchmark case IDs, maps, kinds, and multiplayer player counts"
        in payload["system"]
    )
    assert "4 to 8 short bullets" in payload["system"]
    assert (
        "If the final numerical limit is ambiguous, omit the number"
        in payload["system"]
    )
    assert "CloudKit" not in payload["system"]
    assert "disabled" not in payload["system"].lower()
    facts = json.loads(payload["messages"][0]["content"])
    assert facts["selected_platforms"] == ["ios", "tvos"]
    assert facts["benchmark_upload_enabled"] is False
    assert "two and four players" in facts["changelog_entry"]
    assert [
        case["players"] for case in facts["verified_recording_catalog"]["cases"]
    ] == [
        16,
        32,
    ]


def test_release_summary_instructions_keep_report_sending_policy_conditional() -> None:
    disabled = release.release_summary_instructions(
        False, ["ios"], has_recording_catalog=True
    )
    enabled = release.release_summary_instructions(
        True, ["ios"], has_recording_catalog=True
    )
    absent = release.release_summary_instructions(
        False, ["ios"], has_recording_catalog=False
    )
    assert "The boolean policy value is internal metadata" in disabled
    assert "Mention it only when the changelog clearly describes it" in enabled
    assert "do not infer current benchmark case counts" in absent


@pytest.mark.parametrize(
    "mutate",
    [
        lambda context: context.update(guest_sha256="0" * 64),
        lambda context: context.update(envelope_sha256="0" * 64),
        lambda context: context["cases"][0].update(players=129),
        lambda context: context["cases"][0].update(recording="private.take"),
    ],
)
def test_recording_summary_context_rejects_unbound_or_malformed_facts(
    mutate: Any,
) -> None:
    from tools.release_validation import validate_recording_summary_context

    context = recording_summary_context()
    mutate(context)
    with pytest.raises(ValueError):
        validate_recording_summary_context(json.dumps(context), policy())


@pytest.mark.parametrize("value", ["", "not json", '{"schema": 1}'])
def test_recording_summary_context_requires_complete_preflight_output(
    value: str,
) -> None:
    from tools.release_validation import validate_recording_summary_context

    with pytest.raises(ValueError):
        validate_recording_summary_context(value, policy())


def test_anthropic_summary_requires_wif_configuration_before_network(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    for name in (
        "ACTIONS_ID_TOKEN_REQUEST_URL",
        "ACTIONS_ID_TOKEN_REQUEST_TOKEN",
        "ANTHROPIC_FEDERATION_RULE_ID",
        "ANTHROPIC_ORGANIZATION_ID",
        "ANTHROPIC_SERVICE_ACCOUNT_ID",
        "ANTHROPIC_WORKSPACE_ID",
        "GITHUB_ACTIONS",
        "GITHUB_EVENT_NAME",
        "GITHUB_REF",
        "GITHUB_REPOSITORY",
        "GITHUB_WORKFLOW_REF",
        "GITHUB_JOB",
    ):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("ANTHROPIC_API_KEY", "legacy-key-must-not-be-used")
    monkeypatch.setattr(
        release.urllib.request,
        "build_opener",
        lambda *_args: pytest.fail(
            "provider must not be called without a configured key"
        ),
    )
    with pytest.raises(ValueError, match="Workload identity configuration"):
        release.anthropic_release_summary(
            "## 0.3.0\n\n### feat\n\n- Add maps.",
            version="0.3.0",
            selected_platforms=["ios"],
            benchmark_upload=False,
        )


@pytest.mark.parametrize("value", [None, "", "   "])
def test_release_summary_model_defaults_when_environment_value_is_empty(
    monkeypatch: pytest.MonkeyPatch, value: str | None
) -> None:
    if value is None:
        monkeypatch.delenv("RELEASE_SUMMARY_MODEL", raising=False)
    else:
        monkeypatch.setenv("RELEASE_SUMMARY_MODEL", value)
    assert release.release_summary_model() == "claude-sonnet-5-5"


@pytest.mark.parametrize(
    "response",
    [
        {
            "type": "message",
            "role": "assistant",
            "stop_reason": "max_tokens",
            "content": [{"type": "text", "text": "cut off"}],
        },
        {
            "type": "message",
            "role": "assistant",
            "stop_reason": "refusal",
            "content": [{"type": "refusal", "text": "No."}],
        },
        {
            "type": "message",
            "role": "assistant",
            "stop_reason": "tool_use",
            "content": [{"type": "tool_use", "id": "tool-1", "name": "unused"}],
        },
        {
            "type": "message",
            "role": "assistant",
            "stop_reason": "end_turn",
            "content": [
                {"type": "text", "text": "summary"},
                {"type": "tool_use", "id": "tool-1", "name": "unused"},
            ],
        },
        {
            "type": "message",
            "role": "assistant",
            "stop_reason": "end_turn",
            "content": [{"type": "text", "text": "x" * 4_001}],
        },
        {
            "type": "message",
            "role": "assistant",
            "stop_reason": "end_turn",
            "content": [{"type": "text", "text": "  \n"}],
        },
        {
            "type": "message",
            "role": "assistant",
            "stop_reason": "end_turn",
            "content": [{"type": "text", "text": "  # TestFlight heading"}],
        },
    ],
)
def test_anthropic_summary_fails_closed_on_incomplete_refused_or_oversized_output(
    response: dict[str, Any], monkeypatch: pytest.MonkeyPatch
) -> None:
    body = json.dumps(response).encode()

    class FakeResponse:
        def __enter__(self) -> Self:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, _limit: int = -1) -> bytes:
            return body

    class FakeOpener:
        def open(self, _request: Any, timeout: int) -> FakeResponse:
            assert timeout == 60
            return FakeResponse()

    _stub_wif_access_token(monkeypatch)
    monkeypatch.setattr(
        release.urllib.request, "build_opener", lambda *_handlers: FakeOpener()
    )
    with pytest.raises(ValueError):
        release.anthropic_release_summary(
            "## 0.3.0\n\n### feat\n\n- Add maps.",
            version="0.3.0",
            selected_platforms=["ios", "tvos"],
            benchmark_upload=False,
        )


def test_anthropic_summary_error_does_not_expose_legacy_key_or_response_body(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    secret = "legacy-api-key-must-not-be-used"
    _stub_wif_access_token(monkeypatch)
    monkeypatch.setenv("ANTHROPIC_API_KEY", secret)

    class FakeOpener:
        def open(self, _request: Any, timeout: int) -> Any:
            assert timeout == 60
            raise urllib.error.HTTPError(
                "https://api.anthropic.com/v1/messages",
                503,
                "Unavailable",
                {},
                io.BytesIO(secret.encode()),
            )

    monkeypatch.setattr(
        release.urllib.request, "build_opener", lambda *_handlers: FakeOpener()
    )
    with pytest.raises(ValueError, match="HTTP 503") as error:
        release.anthropic_release_summary(
            "## 0.3.0\n\n### feat\n\n- Add maps.",
            version="0.3.0",
            selected_platforms=["ios", "tvos"],
            benchmark_upload=False,
        )
    assert secret not in str(error.value)


def test_anthropic_summary_accepts_output_between_target_and_hard_limit(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    summary = "x" * 3_500
    body = json.dumps(
        {
            "type": "message",
            "role": "assistant",
            "stop_reason": "end_turn",
            "content": [{"type": "text", "text": summary}],
        }
    ).encode()

    class FakeResponse:
        def __enter__(self) -> Self:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, _limit: int = -1) -> bytes:
            return body

    class FakeOpener:
        def open(self, _request: Any, timeout: int) -> FakeResponse:
            assert timeout == 60
            return FakeResponse()

    _stub_wif_access_token(monkeypatch)
    monkeypatch.setattr(
        release.urllib.request, "build_opener", lambda *_handlers: FakeOpener()
    )
    assert (
        release.anthropic_release_summary(
            "## 0.3.0\n\n### feat\n\n- Add maps.",
            version="0.3.0",
            selected_platforms=["ios", "tvos"],
            benchmark_upload=False,
        )
        == summary
    )


def test_anthropic_summary_rejects_malformed_response_json(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class FakeResponse:
        def __enter__(self) -> Self:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, _limit: int = -1) -> bytes:
            return b"not-json"

    class FakeOpener:
        def open(self, _request: Any, timeout: int) -> FakeResponse:
            assert timeout == 60
            return FakeResponse()

    _stub_wif_access_token(monkeypatch)
    monkeypatch.setattr(
        release.urllib.request, "build_opener", lambda *_handlers: FakeOpener()
    )
    with pytest.raises(ValueError, match="invalid JSON"):
        release.anthropic_release_summary(
            "## 0.3.0\n\n### feat\n\n- Add maps.",
            version="0.3.0",
            selected_platforms=["ios", "tvos"],
            benchmark_upload=False,
        )


def test_anthropic_summary_response_byte_budget_fails_closed(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class FakeResponse:
        def __enter__(self) -> Self:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, limit: int = -1) -> bytes:
            return b"x" * limit

    class FakeOpener:
        def open(self, _request: Any, timeout: int) -> FakeResponse:
            assert timeout == 60
            return FakeResponse()

    _stub_wif_access_token(monkeypatch)
    monkeypatch.setattr(
        release.urllib.request, "build_opener", lambda *_handlers: FakeOpener()
    )
    with pytest.raises(ValueError, match="response exceeded"):
        release.anthropic_release_summary(
            "## 0.3.0\n\n### feat\n\n- Add maps.",
            version="0.3.0",
            selected_platforms=["ios", "tvos"],
            benchmark_upload=False,
        )


def test_anthropic_summary_timeout_fails_without_revealing_provider_data(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class FakeOpener:
        def open(self, _request: Any, timeout: int) -> Any:
            assert timeout == 60
            raise TimeoutError("secret response body and test-api-key")

    _stub_wif_access_token(monkeypatch)
    monkeypatch.setenv("ANTHROPIC_API_KEY", "test-api-key")
    monkeypatch.setattr(
        release.urllib.request, "build_opener", lambda *_handlers: FakeOpener()
    )
    with pytest.raises(ValueError, match="failed or timed out") as error:
        release.anthropic_release_summary(
            "## 0.3.0\n\n### feat\n\n- Add maps.",
            version="0.3.0",
            selected_platforms=["ios", "tvos"],
            benchmark_upload=False,
        )
    assert "test-api-key" not in str(error.value)
    assert "secret response body" not in str(error.value)


def test_anthropic_redirect_handler_refuses_cross_host_redirect() -> None:
    request = release.urllib.request.Request(
        release.ANTHROPIC_MESSAGES_URL,
        data=b"{}",
        headers={"Authorization": "Bearer short-lived-token"},
        method="POST",
    )
    handler = release._NoCredentialRedirect()
    with pytest.raises(urllib.error.HTTPError) as error:
        handler.redirect_request(
            request,
            io.BytesIO(),
            302,
            "Found",
            {},
            "https://attacker.example/collect",
        )
    assert error.value.url == release.ANTHROPIC_MESSAGES_URL


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


def test_publish_uses_summary_for_testflight_and_full_entry_for_github(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    summary = "Try the new campaign and multiplayer cases."
    details = "### feat\n\n- " + ("Preserve complete release detail. " * 150)
    notes = release.release_notes(
        release.insert_testflight_summary("## 0.3.0\n\n" + details, summary),
        "0.3.0",
    )
    manifest = {
        "build_policy": policy(),
        "tag": "visr-v0.3.0",
        "version": "0.3.0",
        "build_number": "20261018.135056",
        "platforms": ["ios"],
        "source_date_epoch": "1792356656",
        "notes_sha256": hashlib.sha256(notes.encode()).hexdigest(),
        "testflight_summary": summary,
        "testflight_summary_sha256": hashlib.sha256(summary.encode()).hexdigest(),
        "testflight_summary_model": "claude-sonnet-5-5",
    }
    (tmp_path / ".release").mkdir()
    (tmp_path / release.MANIFEST).write_text(json.dumps(manifest))
    (tmp_path / "CHANGELOG.md").write_text(notes)
    journal = {"ios": {"state": "complete", "app_id": "app-1", "guest_sha256": "guest"}}
    draft = {
        "id": "release-1",
        "tag_name": manifest["tag"],
        "assets": [],
        "draft": True,
        "body": release.draft_body(notes, journal),
    }
    github = FakeAPI(
        [
            [draft],
            {},
            {},
            {"tag_name": manifest["tag"], "body": notes, "draft": False, "assets": []},
        ]
    )
    apple = FakeAPI(
        [
            build_page(version="0.3.0", build="20261018.135056"),
            build_page(version="0.3.0", build="20261018.135056"),
            [localization(None)],
            {},
            [localization(summary)],
        ]
    )
    monkeypatch.setattr(release, "ROOT", tmp_path)
    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "publish_context", lambda _: (tmp_path, manifest))
    monkeypatch.setattr(release, "run", lambda *_args, **_kwargs: "same-commit")
    monkeypatch.setattr(
        release, "API", lambda base, _token: github if "github" in base else apple
    )
    monkeypatch.setattr(release, "apple_token", lambda: "test-token")
    monkeypatch.setenv("GH_TOKEN", "test-token")
    monkeypatch.setenv("GITHUB_REPOSITORY", "example/app")
    from tools import release_validation

    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(policy()))
    monkeypatch.setattr(release_validation, "toolchain", lambda *_, **__: {})
    monkeypatch.setattr(release_validation, "check_feeds", lambda *_, **__: None)

    release.publish(type("Args", (), {"source_root": tmp_path})())

    testflight_write = next(
        call
        for call in apple.calls
        if call[0] in {"POST", "PATCH"}
        and call[1].startswith("/v1/betaBuildLocalizations")
    )
    assert testflight_write[2]["data"]["attributes"]["whatsNew"] == summary
    github_publish = [
        call
        for call in github.calls
        if call[0] == "PATCH" and call[1].endswith("/release-1")
    ][-1]
    assert github_publish[2]["body"] == notes


def test_prepare_bootstrap_generates_one_version_and_atomic_push(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(release.time, "strftime", lambda *_: "20261016.000000")
    source = "a" * 40
    baseline = "b" * 40
    calls: list[tuple[Any, ...]] = []
    generated_notes = "## visr-v0.1.3\n\n### perf\n\n- Warm maps"
    summary = "Try the newly warmed campaign maps."
    summary_requests: list[tuple[str, str, bool, dict[str, Any]]] = []

    def fake_run(*args: Any, **kwargs: Any) -> str:
        calls.append(args)
        if args[:3] == ("git", "rev-parse", "origin/main"):
            return source
        if args[:3] == ("cz", "bump", "--get-next"):
            return "0.1.3"
        if args[:2] == ("cz", "bump"):
            (tmp_path / "CHANGELOG.md").write_text(generated_notes)
        if args[:3] == ("git", "show", "--no-patch"):
            return "1760576523"
        return ""

    monkeypatch.setattr(release, "ROOT", tmp_path)
    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "run", fake_run)
    monkeypatch.setattr(release, "output", lambda *_args: None)

    def summarize(
        notes: str,
        *,
        version: str,
        selected_platforms: list[str],
        benchmark_upload: bool,
        catalog_context: dict[str, Any],
        model: str | None = None,
    ) -> str:
        summary_requests.append((version, notes, benchmark_upload, catalog_context))
        assert selected_platforms == ["ios", "tvos"]
        assert [case["players"] for case in catalog_context["cases"]] == [16, 32]
        assert model == "claude-sonnet-5-5"
        return summary

    monkeypatch.setattr(release, "anthropic_release_summary", summarize)
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
    monkeypatch.setenv(
        "RELEASE_RECORDING_CONTEXT", json.dumps(recording_summary_context())
    )
    monkeypatch.setenv("RELEASE_SUMMARY_MODEL", "claude-sonnet-5-5")
    from tools.release_validation import preflight_identity

    monkeypatch.setenv(
        "RELEASE_PREFLIGHT_IDENTITY",
        preflight_identity(source, ["ios", "tvos"], policy()),
    )
    release.prepare(args)
    manifest = json.loads((tmp_path / release.MANIFEST).read_text())
    notes = release.release_notes((tmp_path / "CHANGELOG.md").read_text(), "0.1.3")
    assert manifest["version"] == "0.1.3"
    assert manifest["build_number"] == "20261016.0"
    assert manifest["source_sha"] == source
    assert manifest["bootstrap_since"] == baseline
    assert manifest["notes_sha256"] == hashlib.sha256(notes.encode()).hexdigest()
    assert manifest["testflight_summary"] == summary
    assert (
        manifest["testflight_summary_sha256"]
        == hashlib.sha256(summary.encode()).hexdigest()
    )
    assert manifest["testflight_summary_model"] == "claude-sonnet-5-5"
    assert summary_requests == [
        ("0.1.3", generated_notes, False, recording_summary_context())
    ]
    assert release.testflight_summary_for_release(notes, manifest) == summary
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


def test_prepare_resume_reuses_pinned_summary_without_provider_call(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = "a" * 40
    summary = "Play larger multiplayer battles."
    notes = release.insert_testflight_summary(
        "## 0.3.0\n\n### feat\n\n- Add multiplayer cases.", summary
    )
    manifest = {
        "build_policy": policy(),
        "tag": "visr-v0.3.0",
        "version": "0.3.0",
        "build_number": "20261018.135056",
        "source_sha": source,
        "platforms": ["ios", "tvos"],
        "notes_sha256": hashlib.sha256(notes.encode()).hexdigest(),
        "testflight_summary": summary,
        "testflight_summary_sha256": hashlib.sha256(summary.encode()).hexdigest(),
        "testflight_summary_model": "claude-sonnet-5-5",
        "source_date_epoch": "1792356656",
    }
    (tmp_path / ".release").mkdir()
    (tmp_path / release.MANIFEST).write_text(json.dumps(manifest))
    (tmp_path / "CHANGELOG.md").write_text(notes)
    calls: list[tuple[Any, ...]] = []
    outputs: list[tuple[str, str]] = []

    def fake_run(*args: Any, **_kwargs: Any) -> str:
        calls.append(args)
        if args[:3] == ("git", "rev-parse", "HEAD^"):
            return source
        return ""

    monkeypatch.setattr(release, "ROOT", tmp_path)
    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "run", fake_run)
    monkeypatch.setattr(release, "output", lambda *args: outputs.append(args))
    monkeypatch.setattr(
        release,
        "anthropic_release_summary",
        lambda *_args, **_kwargs: pytest.fail("resume must reuse the pinned summary"),
    )
    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(policy()))
    monkeypatch.delenv("RELEASE_RECORDING_CONTEXT", raising=False)
    from tools.release_validation import preflight_identity

    monkeypatch.setenv(
        "RELEASE_PREFLIGHT_IDENTITY",
        preflight_identity(source, ["ios", "tvos"], policy()),
    )
    for name in (
        "ACTIONS_ID_TOKEN_REQUEST_URL",
        "ACTIONS_ID_TOKEN_REQUEST_TOKEN",
        "ANTHROPIC_FEDERATION_RULE_ID",
        "ANTHROPIC_ORGANIZATION_ID",
        "ANTHROPIC_SERVICE_ACCOUNT_ID",
        "ANTHROPIC_WORKSPACE_ID",
        "GITHUB_ACTIONS",
        "GITHUB_EVENT_NAME",
        "GITHUB_REF",
        "GITHUB_REPOSITORY",
        "GITHUB_WORKFLOW_REF",
        "GITHUB_JOB",
    ):
        monkeypatch.delenv(name, raising=False)
    args = type(
        "Args",
        (),
        {
            "source_sha": source,
            "platforms": "ios,tvos",
            "resume_tag": "visr-v0.3.0",
            "bootstrap_since": "",
            "bootstrap_version": "0.1.2",
            "increment": "auto",
        },
    )()

    release.prepare(args)

    assert outputs == [("tag", "visr-v0.3.0")]
    assert not any(call[:2] in [("cz", "bump"), ("git", "push")] for call in calls)


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


def test_prepare_requires_wif_config_before_version_or_tag_changes(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    source = "a" * 40
    calls: list[Any] = []

    def fake_run(*args: Any, **_kwargs: Any) -> str:
        calls.append(args)
        if args[:3] == ("git", "rev-parse", "origin/main"):
            return source
        return ""

    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "run", fake_run)
    monkeypatch.setattr(release, "ROOT", Path("/unused/release-root"))
    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(policy()))
    monkeypatch.setenv(
        "RELEASE_RECORDING_CONTEXT", json.dumps(recording_summary_context())
    )
    monkeypatch.delenv("ANTHROPIC_FEDERATION_RULE_ID", raising=False)
    from tools.release_validation import preflight_identity

    monkeypatch.setenv(
        "RELEASE_PREFLIGHT_IDENTITY", preflight_identity(source, ["ios"], policy())
    )
    args = type(
        "Args",
        (),
        {
            "source_sha": source,
            "platforms": "ios",
            "resume_tag": "",
            "bootstrap_since": "",
            "bootstrap_version": "0.1.2",
            "increment": "auto",
        },
    )()

    with pytest.raises(ValueError, match="Workload identity configuration"):
        release.prepare(args)

    assert not any(
        call[:2] in [("cz", "bump"), ("git", "tag"), ("git", "commit"), ("git", "push")]
        for call in calls
    )


@pytest.mark.parametrize("scope", ["workspace:inference", "workspace:developer"])
def test_workload_identity_exchange_uses_github_assertion_and_workspace_scope(
    monkeypatch: pytest.MonkeyPatch, scope: str
) -> None:
    _configure_wif_environment(monkeypatch)
    assertion = _github_oidc_jwt_fixture()
    access_token = "sk-ant-oat01-fixture-access-token"
    responses = [
        json.dumps({"value": assertion}).encode(),
        json.dumps(
            {
                "access_token": access_token,
                "token_type": "Bearer",
                "expires_in": 600,
                "scope": scope,
            }
        ).encode(),
    ]
    requests: list[Any] = []

    class FakeResponse:
        def __init__(self, body: bytes) -> None:
            self.body = body

        def __enter__(self) -> Self:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, limit: int = -1) -> bytes:
            assert len(self.body) <= limit
            return self.body

    class FakeOpener:
        def open(self, request: Any, timeout: int) -> FakeResponse:
            requests.append((request, timeout))
            return FakeResponse(responses.pop(0))

    token, metadata = release.exchange_anthropic_access_token(opener=FakeOpener())

    assert token == access_token
    assert metadata == {"scope": scope, "expires_in": 600}
    assert len(requests) == 2
    oidc_request, oidc_timeout = requests[0]
    assert oidc_request.full_url.startswith(
        "https://pipelines.actions.githubusercontent.com/test-id?api-version=2&"
    )
    assert "audience=https%3A%2F%2Fapi.anthropic.com" in oidc_request.full_url
    assert (
        oidc_request.get_header("Authorization")
        == "Bearer github-request-token-fixture"
    )
    assert oidc_timeout == release.WIF_TIMEOUT_SECONDS
    exchange_request, exchange_timeout = requests[1]
    assert exchange_request.full_url == "https://api.anthropic.com/v1/oauth/token"
    assert exchange_request.get_header("Content-type") == "application/json"
    assert "Authorization" not in exchange_request.header_items()
    assert exchange_timeout == release.WIF_TIMEOUT_SECONDS
    assert json.loads(exchange_request.data) == {
        "grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer",
        "assertion": assertion,
        "federation_rule_id": "fdrl_test",
        "organization_id": "00000000-0000-0000-0000-000000000000",
        "service_account_id": "svac_test",
        "workspace_id": "wrkspc_test",
    }


def test_auth_only_identity_check_exchanges_without_message_request(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    token = "sk-ant-oat01-do-not-print"
    monkeypatch.setattr(
        release,
        "exchange_anthropic_access_token",
        lambda **_kwargs: (token, {"scope": "workspace:inference", "expires_in": 600}),
    )
    monkeypatch.setattr(
        release,
        "anthropic_release_summary",
        lambda *_args, **_kwargs: pytest.fail("auth-only mode must not call Messages"),
    )

    release.identity_check()

    output = capsys.readouterr().out
    assert "authentication succeeded" in output.lower()
    assert "workspace:inference" in output
    assert token not in output


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("ANTHROPIC_FEDERATION_RULE_ID", ""),
        ("ANTHROPIC_ORGANIZATION_ID", "not-a-uuid"),
        ("ANTHROPIC_SERVICE_ACCOUNT_ID", ""),
        ("ANTHROPIC_WORKSPACE_ID", ""),
        ("ACTIONS_ID_TOKEN_REQUEST_TOKEN", ""),
    ],
)
def test_workload_identity_exchange_rejects_missing_or_malformed_config_before_network(
    monkeypatch: pytest.MonkeyPatch, field: str, value: str
) -> None:
    _configure_wif_environment(monkeypatch)
    monkeypatch.setenv(field, value)
    monkeypatch.setattr(
        release.urllib.request,
        "build_opener",
        lambda *_args: pytest.fail("invalid WIF configuration must fail before HTTP"),
    )

    with pytest.raises(ValueError):
        release.exchange_anthropic_access_token()


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("GITHUB_EVENT_NAME", "push", "restricted to the release workflow_dispatch"),
        (
            "GITHUB_REF",
            "refs/heads/feature",
            "restricted to the release workflow_dispatch",
        ),
        (
            "GITHUB_REPOSITORY",
            "other/repo",
            "restricted to the release workflow_dispatch",
        ),
        (
            "GITHUB_WORKFLOW_REF",
            "other/repo/.github/workflows/release.yml@refs/heads/main",
            "restricted to the release workflow_dispatch",
        ),
        ("GITHUB_JOB", "testflight", "unavailable to this release job"),
    ],
)
def test_workload_identity_requires_exact_release_dispatch_context_before_network(
    monkeypatch: pytest.MonkeyPatch, field: str, value: str, message: str
) -> None:
    _configure_wif_environment(monkeypatch)
    monkeypatch.setenv(field, value)
    monkeypatch.setattr(
        release.urllib.request,
        "build_opener",
        lambda *_args: pytest.fail("untrusted workflow context must fail before HTTP"),
    )

    with pytest.raises(ValueError, match=message):
        release.exchange_anthropic_access_token()


def test_workload_identity_rejects_oidc_claim_mismatch_before_exchange(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _configure_wif_environment(monkeypatch)
    requests: list[Any] = []

    class FakeResponse:
        def __enter__(self) -> Self:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, _limit: int = -1) -> bytes:
            return json.dumps(
                {"value": _github_oidc_jwt_fixture(ref="refs/heads/other")}
            ).encode()

    class FakeOpener:
        def open(self, request: Any, timeout: int) -> FakeResponse:
            requests.append(request)
            return FakeResponse()

    with pytest.raises(ValueError, match="claim validation failed: ref"):
        release.exchange_anthropic_access_token(opener=FakeOpener())
    assert len(requests) == 1


@pytest.mark.parametrize(
    ("claim", "value", "expected_name"),
    [
        ("sub", "repo:steverice/visr:environment:release", "sub"),
        (
            "sub",
            "repo:other@1423851/visr@1408152155:environment:release",
            "sub",
        ),
        ("environment", "other", "environment"),
    ],
)
def test_workload_identity_rejects_wrong_subject_and_reports_claim_name_only(
    claim: str, value: str, expected_name: str
) -> None:
    sentinel = "private-claim-value-must-not-appear"
    supplied = sentinel if value == "other" else value
    with pytest.raises(ValueError) as error:
        release._github_oidc_claims(_github_oidc_jwt_fixture(**{claim: supplied}))
    assert expected_name in str(error.value)
    assert sentinel not in str(error.value)


def test_workload_identity_accepts_github_immutable_subject_claim() -> None:
    assert release._github_oidc_claims(_github_oidc_jwt_fixture())["sub"] == (
        "repo:steverice@1423851/visr@1408152155:environment:release"
    )


def test_workload_identity_rejects_overbroad_scope_without_exposing_token(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _configure_wif_environment(monkeypatch)
    access_token = "sk-ant-oat01-overbroad-fixture-token"
    responses = [
        json.dumps({"value": _github_oidc_jwt_fixture()}).encode(),
        json.dumps(
            {
                "access_token": access_token,
                "token_type": "Bearer",
                "expires_in": 600,
                "scope": "org:admin",
            }
        ).encode(),
    ]

    class FakeResponse:
        def __enter__(self) -> Self:
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self, _limit: int = -1) -> bytes:
            return responses.pop(0)

    class FakeOpener:
        def open(self, _request: Any, timeout: int) -> FakeResponse:
            assert timeout == release.WIF_TIMEOUT_SECONDS
            return FakeResponse()

    with pytest.raises(ValueError, match="over-scoped credentials") as error:
        release.exchange_anthropic_access_token(opener=FakeOpener())
    assert access_token not in str(error.value)


def test_workload_identity_timeout_does_not_expose_github_request_credential(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _configure_wif_environment(monkeypatch)
    request_credential = "github-request-credential-fixture"
    monkeypatch.setenv("ACTIONS_ID_TOKEN_REQUEST_TOKEN", request_credential)

    class FakeOpener:
        def open(self, _request: Any, timeout: int) -> Any:
            assert timeout == release.WIF_TIMEOUT_SECONDS
            raise TimeoutError(request_credential)

    with pytest.raises(ValueError, match="failed or timed out") as error:
        release.exchange_anthropic_access_token(opener=FakeOpener())
    assert request_credential not in str(error.value)


def test_workload_identity_exchange_error_does_not_expose_tokens_or_response_body(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _configure_wif_environment(monkeypatch)
    secret = "fixture-private-provider-response"

    class FakeOpener:
        def open(self, request: Any, timeout: int) -> Any:
            if request.full_url.startswith(
                "https://pipelines.actions.githubusercontent.com/"
            ):
                return type(
                    "Response",
                    (),
                    {
                        "__enter__": lambda self: self,
                        "__exit__": lambda self, *_args: None,
                        "read": lambda self, _limit=-1: json.dumps(
                            {"value": _github_oidc_jwt_fixture()}
                        ).encode(),
                    },
                )()
            raise urllib.error.HTTPError(
                request.full_url,
                401,
                "Unauthorized",
                {},
                io.BytesIO(secret.encode()),
            )

    with pytest.raises(ValueError) as error:
        release.exchange_anthropic_access_token(opener=FakeOpener())
    assert secret not in str(error.value)
    assert "401" in str(error.value)


def test_prepare_requires_preflight_catalog_context_before_version_changes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = "a" * 40
    calls: list[Any] = []

    def fake_run(*args: Any, **_kwargs: Any) -> str:
        calls.append(args)
        if args[:3] == ("git", "rev-parse", "origin/main"):
            return source
        return ""

    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "run", fake_run)
    monkeypatch.setattr(release, "ROOT", tmp_path)
    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(policy()))
    monkeypatch.delenv("RELEASE_RECORDING_CONTEXT", raising=False)
    from tools.release_validation import preflight_identity

    monkeypatch.setenv(
        "RELEASE_PREFLIGHT_IDENTITY", preflight_identity(source, ["ios"], policy())
    )
    args = type(
        "Args",
        (),
        {
            "source_sha": source,
            "platforms": "ios",
            "resume_tag": "",
            "bootstrap_since": "",
            "bootstrap_version": "0.1.2",
            "increment": "auto",
        },
    )()

    with pytest.raises(ValueError, match="catalog context is missing"):
        release.prepare(args)

    assert not any(
        call[:2] in [("cz", "bump"), ("git", "tag"), ("git", "commit"), ("git", "push")]
        for call in calls
    )


def test_prepare_requires_verified_catalog_context_before_version_changes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = "a" * 40
    calls: list[Any] = []

    def fake_run(*args: Any, **_kwargs: Any) -> str:
        calls.append(args)
        if args[:3] == ("git", "rev-parse", "origin/main"):
            return source
        return ""

    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setattr(release, "run", fake_run)
    monkeypatch.setattr(release, "ROOT", tmp_path)
    monkeypatch.setenv("RELEASE_BUILD_POLICY", json.dumps(policy()))
    monkeypatch.delenv("RELEASE_RECORDING_CONTEXT", raising=False)
    from tools.release_validation import preflight_identity

    monkeypatch.setenv(
        "RELEASE_PREFLIGHT_IDENTITY", preflight_identity(source, ["ios"], policy())
    )
    args = type(
        "Args",
        (),
        {
            "source_sha": source,
            "platforms": "ios",
            "resume_tag": "",
            "bootstrap_since": "",
            "bootstrap_version": "0.1.2",
            "increment": "auto",
        },
    )()

    with pytest.raises(ValueError, match="context is missing"):
        release.prepare(args)

    assert not any(
        call[:2] in [("cz", "bump"), ("git", "tag"), ("git", "commit"), ("git", "push")]
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
    github = FakeAPI(
        [
            [draft],
            {},
            {},
            {},
            {"tag_name": manifest["tag"], "body": notes, "draft": False, "assets": []},
        ]
    )
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
    assert github.calls[-2][2] == {
        "tag_name": manifest["tag"],
        "body": notes,
        "draft": False,
        "make_latest": "true",
    }
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
    assert (
        release.benchmark_upload_arguments({**policy(), "benchmark_upload": "off"})
        == []
    )
    assert release.benchmark_upload_arguments(
        {**policy(), "benchmark_upload": "on"}
    ) == ["--benchmark-upload"]
