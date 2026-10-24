"""Independent game performance report policy and cached configuration resets."""
import json
import pytest
from tools import ios_build, macos_build, release, release_validation as validation
from tools.test_macos_build import options
from tools.test_release_validation import policy


@pytest.mark.parametrize("shader", ["on", "off"])
@pytest.mark.parametrize("benchmark", ["on", "off"])
@pytest.mark.parametrize("perf", [None, "off", "on"])
def test_independent_perf_policy(benchmark, perf, shader):
    value = {**policy(), "benchmark_upload": benchmark, "shader_upload": shader}
    if perf is not None:
        value["perf_reports"] = perf
    loaded = validation.load_policy(json.dumps(value))
    assert validation.perf_reports_enabled(loaded) == (perf == "on")
    assert validation.shader_upload_enabled(loaded) == (shader == "on")
    assert validation.benchmark_upload_enabled(loaded) == (benchmark == "on")
    assert release.perf_reports_arguments(loaded) == (["--perf-reports"] if perf == "on" else [])


@pytest.mark.parametrize("invalid", [True, None, "yes", "ON", 1])
def test_invalid_perf_policy(invalid):
    with pytest.raises(ValueError, match="perf_reports"):
        validation.load_policy(json.dumps({**policy(), "perf_reports": invalid}))


@pytest.mark.parametrize("target", ["ios", "tvos", "visionos", "macos"])
def test_unsigned_release_probes_match_perf_policy(target):
    assert "--perf-reports" not in validation.build_command(target)
    command = validation.build_command(target, perf_reports=True)
    assert "--perf-reports" in command
    assert "--benchmark-upload" not in command
    assert "--unsigned" in command


@pytest.mark.parametrize("benchmark", [False, True])
def test_cache_resets_are_explicit_for_both_upload_features(benchmark):
    enabled = ios_build.cloud_definitions("org.steverice.visr", False, None, benchmark, perf_reports=True)
    disabled = ios_build.cloud_definitions("org.steverice.visr", False, None)
    assert "-DHALO_PERF_REPORTS=ON" in enabled
    assert "-DHALO_PERF_REPORTS=OFF" in disabled
    assert "-DHALO_BENCHMARK_UPLOAD=OFF" in disabled
    assert "-DHALO_CLOUD_SAVES=OFF" in enabled
    assert "-DHALO_CLOUD_CONTAINER=iCloud.org.steverice.visr" in enabled


def test_native_perf_build_supports_signed_shipping_and_unsigned_validation():
    configure, command = macos_build.native_commands(options(perf_reports=True, unsigned=True), [])
    assert "-DHALO_PERF_REPORTS=ON" in configure
    assert "CODE_SIGNING_ALLOWED=NO" in command
    # the shipping build signs with an existing profile and never asks Xcode to change the account
    configure, command = macos_build.native_commands(
        options(perf_reports=True, team="TEAM", signing_profile="UUID"), [])
    assert "-DHALO_DEVELOPMENT_TEAM=TEAM" in configure
    assert "CODE_SIGNING_ALLOWED=NO" not in command
    assert "PROVISIONING_PROFILE_SPECIFIER=UUID" in command
    assert not any("ProvisioningUpdates" in item for item in command)
    assert "-DHALO_PERF_REPORTS=OFF" in macos_build.native_commands(options(), [])[0]


def test_perf_evidence_cannot_silently_differ_from_policy():
    pins = {name: "value" for name in ("guest_sha256", "guest_compiler_version", "guest_profile_sha256", "host_profile_sha256")}
    with pytest.raises(ValueError, match="perf-reports"):
        validation.compare_evidence(pins, {**pins, "perf_reports": "on"})
    validation.compare_evidence({**pins, "perf_reports": "on"}, {**pins, "perf_reports": "on"})


def test_perf_summary_pins_disclosure():
    summary = "- Check game performance."
    assert release.perf_reports_summary(summary, {}) == summary
    enabled = release.perf_reports_summary(summary, {"perf_reports": "on"})
    assert release.PERF_REPORTS_DISCLOSURE in enabled
    assert release.perf_reports_summary(enabled, {"perf_reports": "on"}) == enabled
    with pytest.raises(ValueError, match="disclosure"):
        release.testflight_summary_for_release(summary, {"build_policy": {"perf_reports": "on"}})


@pytest.mark.parametrize("target", ["ios", "tvos", "visionos", "macos"])
def test_shipping_commands_include_independent_perf_policy(target, monkeypatch, tmp_path):
    monkeypatch.setenv("APPLE_TEAM_ID", "TEAM")
    monkeypatch.setenv("APPLE_BUNDLE_ID", "org.example.visr")
    manifest = {"version": "1.0", "build_number": "1"}
    enabled = release.platform_build_command(target, manifest, {**policy(), "perf_reports": "on"}, "profile", tmp_path)
    disabled = release.platform_build_command(target, manifest, policy(), "profile", tmp_path)
    assert "--perf-reports" in enabled
    assert "--perf-reports" not in disabled
    assert "--shader-upload" not in enabled
    assert "--benchmark-upload" not in enabled
