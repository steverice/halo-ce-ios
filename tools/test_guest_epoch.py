"""A pinned guest timestamp must be visible to Ninja and isolated from the host."""
import json
import os
import shlex
import shutil
import subprocess

import pytest

from tools import ios_guest_build, release_validation
from tools.test_release_validation import policy


def test_policy_accepts_a_separate_canonical_guest_epoch():
    pins = {**policy(), "guest_source_date_epoch": "1792392070"}
    assert release_validation.load_policy(json.dumps(pins)) == pins


def test_guest_epoch_changes_rebuild_ninja_without_changing_host_epoch(tmp_path, monkeypatch):
    ninja, compiler = shutil.which("ninja"), shutil.which("cc")
    if not ninja or not compiler:
        pytest.skip("requires Ninja and a C compiler")
    monkeypatch.setenv("SOURCE_DATE_EPOCH", "172800")
    source = tmp_path / "input.c"
    source.write_text('#include <stdio.h>\nint main(void) { puts(__DATE__ " " __TIME__); }\n')
    prefix = ios_guest_build.guest_epoch_prefix

    def build(epoch):
        (tmp_path / "build.ninja").write_text(
            "rule guest\n"
            f"  command = {prefix(epoch)}{shlex.quote(compiler)} $in -o $out\n"
            "  description = GUEST\n"
            "rule host\n"
            f"  command = {shlex.quote(compiler)} $in -o $out\n"
            "  description = HOST\n"
            "build guest: guest input.c\n"
            "build host: host input.c\n"
        )
        return subprocess.check_output([ninja], cwd=tmp_path, text=True)

    def output(name):
        return subprocess.check_output([str(tmp_path / name)], text=True).strip()

    assert "GUEST" in build("1")
    assert output("guest") == "Jan  1 1970 00:00:01"
    assert output("host") == "Jan  3 1970 00:00:00"
    rebuilt = build("86401")
    assert "GUEST" in rebuilt and "HOST" not in rebuilt
    assert output("guest") == "Jan  2 1970 00:00:01"
    assert output("host") == "Jan  3 1970 00:00:00"
    assert "no work to do" in build("86401").lower()
    assert os.environ["SOURCE_DATE_EPOCH"] == "172800"


def test_epoch_change_invalidates_cached_ninja_target(tmp_path):
    ninja, compiler = shutil.which("ninja"), shutil.which("cc")
    if not ninja or not compiler:
        pytest.skip("requires Ninja and a C compiler")
    (tmp_path / "input.c").write_text('#include <stdio.h>\nint main(void) { puts(__DATE__ " " __TIME__); }\n')
    prefix = ios_guest_build.guest_epoch_prefix

    def build(epoch):
        (tmp_path / "build.ninja").write_text(
            f"rule guest\n  command = {prefix(epoch)}{shlex.quote(compiler)} $in -o $out\n"
            "  description = GUEST\nbuild guest: guest input.c\n"
        )
        return subprocess.check_output([ninja], cwd=tmp_path, env={**os.environ, "SOURCE_DATE_EPOCH": epoch}, text=True)

    assert "GUEST" in build("1")
    assert subprocess.check_output([str(tmp_path / "guest")], text=True).strip() == "Jan  1 1970 00:00:01"
    assert "GUEST" in build("86401")
    assert subprocess.check_output([str(tmp_path / "guest")], text=True).strip() == "Jan  2 1970 00:00:01"
    assert "no work to do" in build("86401").lower()


@pytest.mark.parametrize("value", [None, True, 1, 1.5, "", "0", "01", "-1", " 1", "1\n", "1;false", "253402300800"])
def test_policy_rejects_noncanonical_guest_epoch(value):
    with pytest.raises(ValueError, match="epoch"):
        release_validation.load_policy(json.dumps({**policy(), "guest_source_date_epoch": value}))


def test_manifest_guest_epoch_is_immutable_and_legacy_fallback_is_recorded(monkeypatch):
    from tools import release
    monkeypatch.setenv("SOURCE_DATE_EPOCH", "999999")
    old = {"source_date_epoch": "172800", "build_policy": policy()}
    assert release.manifest_guest_epoch(old) == "172800"
    new = {**old, "guest_source_date_epoch": "1", "build_policy": {**policy(), "guest_source_date_epoch": "1"}}
    assert release.manifest_guest_epoch(new) == "1"
    for invalid in [
        {**old, "guest_source_date_epoch": "1"},
        {**old, "build_policy": new["build_policy"]},
        {**new, "guest_source_date_epoch": "2"},
    ]:
        with pytest.raises(ValueError, match="epoch"):
            release.manifest_guest_epoch(invalid)


def test_new_release_requires_guest_pin():
    with pytest.raises(ValueError, match="epoch"):
        release_validation.require_guest_epoch_policy(policy())
    assert release_validation.require_guest_epoch_policy({"guest_source_date_epoch": "1"}) == "1"


def test_probe_separates_truthful_source_epoch_and_guest_build_environment(tmp_path, monkeypatch):
    source = "1" * 40
    builds = []

    def run(*args, **kwargs):
        if args[:3] == ("git", "rev-parse", "HEAD"):
            return source
        if args[:3] == ("git", "show", "--no-patch"):
            return "172800"
        builds.append(kwargs["env"])
        return ""

    monkeypatch.setenv("SOURCE_DATE_EPOCH", "999999")
    monkeypatch.setattr(release_validation, "run", run)
    monkeypatch.setattr(release_validation, "toolchain", lambda *_: {})
    monkeypatch.setattr(release_validation, "inspect_build", lambda *_, **kwargs: {
        "guest_sha256": "a" * 64, "guest_source_date_epoch": kwargs["guest_source_date_epoch"]})
    report = release_validation.probe(tmp_path, source, ["ios", "tvos"], guest_source_date_epoch="1")
    assert report["source_date_epoch"] == "172800"
    assert report["guest_source_date_epoch"] == "1"
    assert all(env["SOURCE_DATE_EPOCH"] == "172800" and env["VISR_GUEST_SOURCE_DATE_EPOCH"] == "1" for env in builds)
    assert os.environ["SOURCE_DATE_EPOCH"] == "999999"


def test_release_rejects_probe_epoch_conflict_before_feed_or_signing_checks():
    pins = {**policy(), "guest_source_date_epoch": "1"}
    with pytest.raises(ValueError, match="epoch"):
        release_validation.check_release({"guest_source_date_epoch": "2"}, pins, ["ios"])
    with pytest.raises(ValueError, match="epoch"):
        release_validation.compare_evidence({"guest_source_date_epoch": "2"}, pins)


@pytest.mark.parametrize("mode", ["release", "auth", ""])
def test_unsigned_epoch_override_cannot_enter_release_mode(tmp_path, monkeypatch, mode):
    from tools import release
    monkeypatch.setattr(release, "require_ci", lambda: None)
    monkeypatch.setenv("RELEASE_WORKFLOW_MODE", mode)
    monkeypatch.setattr("sys.argv", ["release_validation", "probe", "--source-sha", "1" * 40,
                                    "--report", str(tmp_path / "report.json"),
                                    "--guest-source-date-epoch", "1"])
    with pytest.raises(ValueError, match="only for unsigned validate"):
        release_validation.main()
    assert not (tmp_path / "report.json").exists()


def test_nonapple_generator_does_not_consume_guest_epoch(monkeypatch):
    from types import SimpleNamespace
    monkeypatch.setenv("VISR_GUEST_SOURCE_DATE_EPOCH", "invalid")
    ios_guest_build.generate_ios_guest_build(None, SimpleNamespace(ios=False))


@pytest.mark.parametrize("rule_name", ["guest_cc", "guest_bc"])
def test_actual_generated_guest_rules_rebuild_when_epoch_changes(tmp_path, monkeypatch, rule_name):
    from pathlib import Path
    from types import SimpleNamespace
    clang, ninja = shutil.which("clang"), shutil.which("ninja")
    if not clang or not ninja:
        pytest.skip("requires Clang and Ninja")
    root = Path(__file__).resolve().parents[1]
    monkeypatch.chdir(root)
    monkeypatch.setattr(ios_guest_build, "fetch_third_party", lambda: None)
    monkeypatch.setattr(ios_guest_build, "xdk_headers", lambda: [])
    (tmp_path / "tools").mkdir()
    (tmp_path / "tools/ios_asm_convert.py").symlink_to(root / "tools/ios_asm_convert.py")
    (tmp_path / "input.c").write_text('const char guest_date[] = __DATE__ " " __TIME__;\n')

    class Captured(Exception):
        pass

    class CaptureRules:
        def comment(self, *_args, **_kwargs):
            pass
        variable = comment
        build = comment

        def rule(self, name, **kwargs):
            rules[name] = kwargs
            if name == "guest_as":
                raise Captured

    def build(epoch):
        monkeypatch.setenv("VISR_GUEST_SOURCE_DATE_EPOCH", epoch)
        rules.clear()
        with pytest.raises(Captured):
            ios_guest_build.generate_ios_guest_build(CaptureRules(), SimpleNamespace(
                ios=True, ios_llvm="/unused", ios_lld="/unused", compiler_launcher=None))
        command = rules[rule_name]["command"]
        (tmp_path / "build.ninja").write_text(
            f"guest_cc = {shlex.quote(clang)}\npython_converter = {shlex.quote(shutil.which('python3') or 'python3')}\n"
            "cflags = --target=arm64-apple-ios16.0 -ffreestanding\n"
            f"rule guest\n  command = {command}\n  description = GUEST\n"
            "  depfile = $out.d\n  deps = gcc\nbuild guest.o: guest input.c\n"
        )
        return subprocess.check_output([ninja, "guest.o"], cwd=tmp_path, text=True)

    def contains_date(value):
        if rule_name == "guest_cc":
            return value.encode() in (tmp_path / "guest.o").read_bytes()
        subprocess.run([clang, "--target=arm64-apple-ios16.0", "-S", "-emit-llvm", "-x", "ir", "guest.o", "-o", "guest.ll"],
                       cwd=tmp_path, check=True, capture_output=True)
        return value in (tmp_path / "guest.ll").read_text()

    rules = {}
    assert "GUEST" in build("1")
    assert contains_date("Jan  1 1970 00:00:01")
    assert "GUEST" in build("86401")
    assert contains_date("Jan  2 1970 00:00:01")
    assert "no work to do" in build("86401").lower()
