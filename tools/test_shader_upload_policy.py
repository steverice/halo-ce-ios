"""Independent shader contribution policy and cached configuration reset contracts."""
import json
import plistlib
from pathlib import Path
import pytest
from tools import ios_build, macos_build, release, release_validation as validation
from tools.test_macos_build import options
from tools.test_release_validation import policy


@pytest.mark.parametrize("benchmark", ["on", "off"])
@pytest.mark.parametrize("shader", [None, "off", "on"])
def test_independent_shader_policy(benchmark, shader):
    value = {**policy(), "benchmark_upload": benchmark}
    if shader is not None:
        value["shader_upload"] = shader
    loaded = validation.load_policy(json.dumps(value))
    assert validation.shader_upload_enabled(loaded) == (shader == "on")
    assert validation.benchmark_upload_enabled(loaded) == (benchmark == "on")
    assert release.shader_upload_arguments(loaded) == (["--shader-upload"] if shader == "on" else [])


@pytest.mark.parametrize("invalid", [True, None, "yes", "ON", 1])
def test_invalid_shader_policy(invalid):
    with pytest.raises(ValueError, match="shader_upload"):
        validation.load_policy(json.dumps({**policy(), "shader_upload": invalid}))


@pytest.mark.parametrize("target", ["ios", "tvos", "visionos"])
def test_unsigned_release_probes_match_shader_policy(target):
    assert "--shader-upload" not in validation.build_command(target)
    command = validation.build_command(target, shader_upload=True)
    assert "--shader-upload" in command
    assert "--benchmark-upload" not in command
    assert "--unsigned" in command


@pytest.mark.parametrize("benchmark", [False, True])
def test_cache_resets_are_explicit_for_both_upload_features(benchmark):
    enabled = ios_build.cloud_definitions("org.steverice.visr", False, None, benchmark, True)
    disabled = ios_build.cloud_definitions("org.steverice.visr", False, None)
    assert "-DHALO_SHADER_UPLOAD=ON" in enabled
    assert "-DHALO_SHADER_UPLOAD=OFF" in disabled
    assert "-DHALO_BENCHMARK_UPLOAD=OFF" in disabled
    assert "-DHALO_CLOUD_SAVES=OFF" in enabled
    assert "-DHALO_CLOUD_CONTAINER=iCloud.org.steverice.visr" in enabled


def test_native_shader_build_supports_signed_shipping_and_unsigned_validation():
    configure, command = macos_build.native_commands(options(shader_upload=True, unsigned=True), [])
    assert "-DHALO_SHADER_UPLOAD=ON" in configure
    assert "CODE_SIGNING_ALLOWED=NO" in command
    # the shipping build signs with an existing profile and never asks Xcode to change the account
    configure, command = macos_build.native_commands(
        options(shader_upload=True, team="TEAM", signing_profile="UUID"), [])
    assert "-DHALO_DEVELOPMENT_TEAM=TEAM" in configure
    assert "CODE_SIGNING_ALLOWED=NO" not in command
    assert "PROVISIONING_PROFILE_SPECIFIER=UUID" in command
    assert not any("ProvisioningUpdates" in item for item in command)
    assert "-DHALO_SHADER_UPLOAD=OFF" in macos_build.native_commands(options(), [])[0]


def test_cloudkit_diagnostics_are_linked_for_every_upload_manifest():
    root = Path(__file__).resolve().parents[1]
    for name in ("tester", "shader"):
        manifest = plistlib.loads((root / "port/ios/privacy" / name / "PrivacyInfo.xcprivacy").read_bytes())
        assert manifest["NSPrivacyTracking"] is False
        entries = manifest["NSPrivacyCollectedDataTypes"]
        types = {item["NSPrivacyCollectedDataType"] for item in entries}
        assert {"NSPrivacyCollectedDataTypeOtherDiagnosticData",
                "NSPrivacyCollectedDataTypeProductInteraction"} <= types
        for entry in entries:
            assert entry["NSPrivacyCollectedDataTypeLinked"] is True
            assert entry["NSPrivacyCollectedDataTypeTracking"] is False
            assert entry["NSPrivacyCollectedDataTypePurposes"] == ["NSPrivacyCollectedDataTypePurposeAnalytics"]


def test_shader_summary_pins_exact_disclosure_and_legacy_text():
    summary = "- Check map loading."
    assert release.shader_upload_summary(summary, {}) == summary
    enabled = release.shader_upload_summary(summary, {"shader_upload": "on"})
    assert release.SHADER_UPLOAD_DISCLOSURE in enabled
    assert release.shader_upload_summary(enabled, {"shader_upload": "on"}) == enabled
    with pytest.raises(ValueError, match="disclosure"):
        release.testflight_summary_for_release(summary, {"build_policy": {"shader_upload": "on"}})


def test_shader_evidence_cannot_silently_differ_from_policy():
    pins = {name: "value" for name in ("guest_sha256", "guest_compiler_version", "guest_profile_sha256", "host_profile_sha256")}
    with pytest.raises(ValueError, match="shader-upload"):
        validation.compare_evidence(pins, {**pins, "shader_upload": "on"})
    validation.compare_evidence({**pins, "shader_upload": "on"}, {**pins, "shader_upload": "on"})


@pytest.mark.parametrize("native", [False, True])
@pytest.mark.parametrize("environment", ["development", "production"])
def test_generated_upload_entitlements_use_the_platform_key_and_reset(tmp_path, native, environment):
    import subprocess

    root = Path(__file__).resolve().parents[1]
    output = tmp_path / "cloud.entitlements"
    base = tmp_path / "base.entitlements"
    base.write_bytes(plistlib.dumps({"example.base-capability": True}))
    script = tmp_path / "generate.cmake"
    script.write_text(
        f'include("{root / "port/ios/CloudUploadEntitlements.cmake"}")\n'
        f'set(HALO_NATIVE_MAC {"ON" if native else "OFF"})\n'
        'set(HALO_SHADER_UPLOAD ON)\n'
        f'set(HALO_BENCHMARK_APS_ENVIRONMENT "{environment}")\n'
        f'halo_cloud_upload_entitlements("{output}" "{base}")\n'
    )
    subprocess.run(["cmake", "-P", str(script)], check=True)
    entitlements = plistlib.loads(output.read_bytes())
    key = "com.apple.developer.aps-environment" if native else "aps-environment"
    other = "aps-environment" if native else "com.apple.developer.aps-environment"
    assert entitlements[key] == environment
    assert other not in entitlements
    assert entitlements["example.base-capability"] is True
    assert entitlements["com.apple.developer.icloud-services"] == ["CloudKit"]
    # Reuse the same generated output after both upload switches turn off.
    script.write_text(script.read_text().replace("set(HALO_SHADER_UPLOAD ON)", "set(HALO_SHADER_UPLOAD OFF)"))
    subprocess.run(["cmake", "-P", str(script)], check=True)
    entitlements = plistlib.loads(output.read_bytes())
    assert key not in entitlements and other not in entitlements


def test_shader_only_container_definition_does_not_enable_benchmark_sender():
    cmake = (Path(__file__).resolve().parents[1] / "port/ios/CMakeLists.txt").read_text()
    start = cmake.index("if(HALO_SHADER_UPLOAD)\n    target_link_libraries(VISR PRIVATE sqlite3)")
    shader = cmake[start:cmake.index("\nendif()", start)]
    assert "HALO_SHADER_UPLOAD=1" in shader
    assert "HALO_SHADER_CLOUD=" in shader
    assert "HALO_BENCHMARK_CLOUD=" not in shader
    start = cmake.index("if(HALO_BENCHMARK_UPLOAD)\n    target_sources")
    benchmark = cmake[start:cmake.index("\nendif()", start)]
    assert "host/host_benchmark_send.m" in benchmark
    assert "HALO_BENCHMARK_CLOUD=" in benchmark
    assert "HALO_SHADER_CLOUD=" not in benchmark
