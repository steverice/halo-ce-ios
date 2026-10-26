"""Tests for tools/ios_build.py's pure helpers."""
from __future__ import annotations

from pathlib import Path

import pytest

from tools import ios_build


def test_ipad_multitasking_declares_all_distribution_required_orientations():
    import plistlib

    info = plistlib.loads((ios_build.ROOT / 'port/ios/Info.plist').read_bytes())
    landscape = {'UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'}
    assert set(info['UISupportedInterfaceOrientations']) == landscape | {'UIInterfaceOrientationPortrait'}
    if not info.get('UIRequiresFullScreen', False):
        ipad = info.get('UISupportedInterfaceOrientations~ipad', info['UISupportedInterfaceOrientations'])
        assert set(ipad) >= landscape | {
            'UIInterfaceOrientationPortrait', 'UIInterfaceOrientationPortraitUpsideDown',
        }

# the iPhoneOS SDK's OpenGLES.tbd as Xcode 27 ships it, cut down to two symbols
STUB = """--- !tapi-tbd
tbd-version:     4
targets:         [ arm64e-ios, arm64e.x1-ios ]
install-name:    '/System/Library/Frameworks/OpenGLES.framework/OpenGLES'
exports:
  - targets:         [ arm64e-ios, arm64e.x1-ios ]
    symbols:         [ _EAGLGetVersion, _glActiveTexture ]
...
"""


@pytest.mark.parametrize("targets", [
    "[ arm64e-ios, arm64e.x1-ios ]",
    "[ armv7-ios, armv7s-ios, arm64-ios, arm64e-ios ]",
    "[ arm64e-ios ]",
])
def test_the_stub_links_against_the_mac_s_maccatalyst_opengles(targets):
    # Each fixture repeats the same target list at the top and in exports.
    source = STUB.replace("[ arm64e-ios, arm64e.x1-ios ]", targets)
    stub = ios_build.maccatalyst_stub(source)
    assert stub.count("[ arm64-maccatalyst, arm64e-maccatalyst ]") == 2
    assert "-ios" not in stub
    assert "install-name:    '/System/iOSSupport/System/Library/Frameworks/OpenGLES.framework/OpenGLES'" in stub
    assert "_EAGLGetVersion, _glActiveTexture" in stub


def test_a_stub_with_other_targets_is_refused():
    with pytest.raises(ValueError, match="does not list"):
        ios_build.maccatalyst_stub(STUB.replace("arm64e.x1-ios", "arm64e.x2-ios"))


@pytest.mark.parametrize("targets", [
    "[ arm64-ios ]", "[ arm64e.x1-ios ]", "[ arm64e.x2-ios ]",
    "[ arm64e-ios, arm64e-ios ]", "[ arm64e-ios, arm64-ios ]",
])
def test_singleton_support_does_not_accept_other_target_sets(targets):
    source = STUB.replace("[ arm64e-ios, arm64e.x1-ios ]", targets)
    with pytest.raises(ValueError, match="iOS target"):
        ios_build.maccatalyst_stub(source)


def test_singleton_stub_requires_matching_export_targets():
    source = STUB.replace("targets:         [ arm64e-ios, arm64e.x1-ios ]",
                          "targets:         [ arm64e-ios ]", 1)
    with pytest.raises(ValueError, match="iOS target"):
        ios_build.maccatalyst_stub(source)


def test_a_stub_with_another_install_name_is_refused():
    with pytest.raises(ValueError, match="install name"):
        ios_build.maccatalyst_stub(STUB.replace("/System/Library/Frameworks/OpenGLES", "/usr/lib/OpenGLES"))


def test_a_stub_naming_an_ios_target_the_rewrite_misses_is_refused():
    with pytest.raises(ValueError, match="iOS target"):
        ios_build.maccatalyst_stub(STUB + "uuids:\n  - target: arm64e-ios\n")


def test_a_stub_with_an_unknown_export_target_list_is_refused():
    source = STUB.replace("  - targets:         [ arm64e-ios, arm64e.x1-ios ]",
                          "  - targets:         [ arm64e-ios, arm64e.x2-ios ]")
    with pytest.raises(ValueError, match="iOS target"):
        ios_build.maccatalyst_stub(source)


@pytest.mark.parametrize("targets", [
    "[arm64e-ios,arm64e.x1-ios]",
    "[ arm64e.x1-ios,\n arm64e-ios ]",
    "[arm64-ios, armv7s-ios,\narm64e-ios, armv7-ios]",
])
def test_stub_target_lists_allow_formatting_and_order_changes(targets):
    source = STUB.replace("[ arm64e-ios, arm64e.x1-ios ]", targets)
    stub = ios_build.maccatalyst_stub(source)
    assert stub.count("[ arm64-maccatalyst, arm64e-maccatalyst ]") == 2
    assert "-ios" not in stub
    assert "_EAGLGetVersion, _glActiveTexture" in stub


def test_unknown_targets_report_version_and_all_target_lists_without_symbols():
    source = STUB.replace("  - targets:         [ arm64e-ios, arm64e.x1-ios ]",
                          "  - targets:         [ arm64e-ios, arm64e.x2-ios ]")
    with pytest.raises(ValueError) as error:
        ios_build.maccatalyst_stub(source)
    message = str(error.value)
    assert "tbd-version=4" in message
    assert "arm64e.x1-ios" in message
    assert "arm64e.x2-ios" in message
    assert "_EAGLGetVersion" not in message


def test_stub_with_different_known_export_architectures_is_refused():
    source = STUB.replace("  - targets:         [ arm64e-ios, arm64e.x1-ios ]",
                          "  - targets:         [ armv7-ios, armv7s-ios, arm64-ios, arm64e-ios ]")
    with pytest.raises(ValueError, match="iOS target"):
        ios_build.maccatalyst_stub(source)


def test_an_unrecognized_json_stub_reports_its_target_metadata():
    source = ('{"tapi_tbd_version": 5, "main_library": {"target_info": '
              '[{"target": "arm64-ios"}], "symbols": ["_private_symbol"]}}')
    with pytest.raises(ValueError) as error:
        ios_build.maccatalyst_stub(source)
    assert "tbd-version=5" in str(error.value)
    assert "arm64-ios" in str(error.value)
    assert "_private_symbol" not in str(error.value)


def test_the_catalyst_build_goes_in_the_checkout_by_default():
    assert ios_build.mac_build_folder({}) == ios_build.ROOT / "build/mac/app"


def test_a_host_s_designated_folder_takes_the_catalyst_build(tmp_path):
    assert ios_build.mac_build_folder({"HALO_MAC_BUILD": str(tmp_path)}) == tmp_path / ios_build.ROOT.name / "app"


def test_an_app_store_package_is_archived_then_exported_and_uploads_only_when_asked(tmp_path):
    import plistlib
    archive, export = ios_build.app_store_commands(tmp_path / "build", "tvOS", "TEAM123456", tmp_path / "out", False)
    assert archive[0] == "xcodebuild" and archive[-1] == "archive" and "generic/platform=tvOS" in archive
    assert "-allowProvisioningUpdates" in archive and "-exportArchive" in export
    options = plistlib.loads((tmp_path / "out" / "ExportOptions.plist").read_bytes())
    assert options["method"] == "app-store-connect" and options["destination"] == "export" and options["teamID"] == "TEAM123456"
    # the build stamps its own version: Xcode must not renumber it
    assert options["manageAppVersionAndBuildNumber"] is False
    ios_build.app_store_commands(tmp_path / "build", "tvOS", "TEAM123456", tmp_path / "out", True)
    assert plistlib.loads((tmp_path / "out" / "ExportOptions.plist").read_bytes())["destination"] == "upload"
    assert "testFlightInternalTestingOnly" not in options
    ios_build.app_store_commands(tmp_path / "build", "iOS", "TEAM123456", tmp_path / "out", True, True)
    internal = plistlib.loads((tmp_path / "out" / "ExportOptions.plist").read_bytes())
    assert internal["testFlightInternalTestingOnly"] is True
    assert internal["destination"] == "upload"


def test_cloud_saves_are_off_unless_asked_for():
    assert ios_build.cloud_definitions("org.steverice.visr", False, None) == [
        "-DHALO_CLOUD_SAVES=OFF", "-DHALO_BENCHMARK_UPLOAD=OFF", "-DHALO_SHADER_UPLOAD=OFF", "-DHALO_PERF_REPORTS=OFF"]


def test_the_project_s_bundle_ids_share_its_container():
    for bundle_id in ("org.steverice.visr", "org.steverice.visr.tvos", "org.steverice.visr.visionos", "org.steverice.visr.mac"):
        assert ios_build.cloud_definitions(bundle_id, True, None) == [
            "-DHALO_CLOUD_SAVES=ON", "-DHALO_CLOUD_CONTAINER=iCloud.org.steverice.visr", "-DHALO_BENCHMARK_UPLOAD=OFF", "-DHALO_SHADER_UPLOAD=OFF", "-DHALO_PERF_REPORTS=OFF"]


def test_another_bundle_id_must_name_its_own_container():
    with pytest.raises(ValueError, match="needs --cloud-container"):
        ios_build.cloud_definitions("com.example.visr", True, None)
    with pytest.raises(ValueError, match="needs --cloud-container"):
        ios_build.cloud_definitions("org.steverice.visrx", True, None)
    assert ios_build.cloud_definitions("com.example.visr", True, "iCloud.com.example.visr")[1] == \
        "-DHALO_CLOUD_CONTAINER=iCloud.com.example.visr"


def test_a_container_that_is_not_an_icloud_identifier_is_refused():
    for container in ("com.example.visr", "iCloud.", "iCloud.visr", "iCloud.com.example.visr;rm"):
        with pytest.raises(ValueError, match="must look like"):
            ios_build.cloud_definitions("com.example.visr", True, container)


def test_a_container_without_the_option_is_refused():
    with pytest.raises(ValueError, match="for --cloud-saves builds"):
        ios_build.cloud_definitions("org.steverice.visr", False, "iCloud.org.steverice.visr")


def test_benchmark_upload_takes_the_container_without_turning_cloud_saves_on():
    assert ios_build.cloud_definitions("org.steverice.visr.tvos", False, None, True) == [
        "-DHALO_CLOUD_SAVES=OFF", "-DHALO_CLOUD_CONTAINER=iCloud.org.steverice.visr", "-DHALO_BENCHMARK_UPLOAD=ON", "-DHALO_SHADER_UPLOAD=OFF", "-DHALO_PERF_REPORTS=OFF"]
    assert ios_build.cloud_definitions("org.steverice.visr.tvos", True, None, True)[0] == "-DHALO_CLOUD_SAVES=ON"
    with pytest.raises(ValueError, match="needs --cloud-container"):
        ios_build.cloud_definitions("com.example.visr", False, None, True)


def test_data_pack_networking_defaults_are_explicitly_empty():
    assert ios_build.data_pack_definitions('', '', '') == [
        '-DHALO_DATA_PACK_PUBLIC_KEY=', '-DHALO_PROFILE_FEED_URL=', '-DHALO_RECORDING_FEED_URL=', '-DHALO_SHADER_FEED_URL=']


def test_data_pack_configuration_accepts_public_key_and_https_feeds():
    assert ios_build.data_pack_definitions('AB' * 32, 'https://packs.example/profiles.json',
                                           'https://packs.example/recordings.json') == [
        '-DHALO_DATA_PACK_PUBLIC_KEY=' + 'ab' * 32,
        '-DHALO_PROFILE_FEED_URL=https://packs.example/profiles.json',
        '-DHALO_RECORDING_FEED_URL=https://packs.example/recordings.json', '-DHALO_SHADER_FEED_URL=']


@pytest.mark.parametrize('key', ['ab' * 31, 'ab' * 33, 'zz' * 32, 'ab' * 32 + '\n'])
def test_data_pack_public_key_must_be_exactly_32_hex_bytes(key):
    with pytest.raises(ValueError, match='public key'):
        ios_build.data_pack_definitions(key, '', '')


def test_mac_marketing_version_reaches_cmake_configuration(monkeypatch, tmp_path):
    import plistlib

    configure = configure_with(monkeypatch, tmp_path, '--mac', '--marketing-version', '2.4.6')
    assert '-DHALO_MARKETING_VERSION=2.4.6' in configure
    mac_info = plistlib.loads((Path(__file__).resolve().parents[1] / 'port/ios/Info-macOS.plist').read_bytes())
    assert mac_info['CFBundleShortVersionString'] == '$(HALO_MARKETING_VERSION)'


def test_metal_renderer_selection_skips_gl_headers_and_catalyst_framework(monkeypatch, tmp_path):
    import sys

    commands = []
    framework_calls = []
    monkeypatch.setattr(ios_build, 'ROOT', tmp_path)
    monkeypatch.setattr(ios_build, 'HEADERS', {'GLES3/gl32.h': ('registry', 'revision', 'source')})
    monkeypatch.setattr(ios_build.platform, 'system', lambda: 'Darwin')
    monkeypatch.setattr(ios_build.platform, 'machine', lambda: 'arm64')
    monkeypatch.setattr(ios_build, 'run', lambda *command: commands.append([str(part) for part in command]))
    monkeypatch.setattr(ios_build.shutil, 'copyfile', lambda *args: None)
    monkeypatch.setattr(ios_build, 'opengles_framework', lambda *args: framework_calls.append(args))
    monkeypatch.setattr(ios_build.urllib.request, 'urlopen', lambda *args, **kwargs: pytest.fail('unexpected GLES header download'))
    monkeypatch.setattr(sys, 'argv', ['ios_build.py', '--mac', '--renderer', 'metal', '--host-pgo=off'])

    ios_build.main()

    configure = next(command for command in commands if command[0] == 'cmake' and '-S' in command)
    assert '-DHALO_RENDERER=METAL' in configure
    assert not framework_calls


def test_default_renderer_selection_keeps_both_backends_except_on_visionos(monkeypatch, tmp_path):
    import sys

    configure = configure_with(monkeypatch, tmp_path, '--unsigned')
    assert '-DHALO_RENDERER=BOTH' in configure

    configure = configure_with(monkeypatch, tmp_path, '--simulator', '--visionos')
    assert '-DHALO_RENDERER=METAL' in configure


def test_non_metal_visionos_renderer_choice_is_rejected(monkeypatch, tmp_path, capsys):
    import sys

    monkeypatch.setattr(ios_build, 'ROOT', tmp_path)
    monkeypatch.setattr(ios_build.platform, 'system', lambda: 'Darwin')
    monkeypatch.setattr(ios_build.platform, 'machine', lambda: 'arm64')
    monkeypatch.setattr(sys, 'argv', ['ios_build.py', '--simulator', '--visionos', '--renderer', 'both'])

    with pytest.raises(SystemExit) as failure:
        ios_build.main()

    assert failure.value.code == 2
    assert '--visionos supports only --renderer metal' in capsys.readouterr().err


def test_metal_only_cmake_gate_covers_gl_backend_sources_headers_and_link():
    cmake = (ios_build.ROOT / 'port/ios/CMakeLists.txt').read_text()

    assert 'if(NOT HALO_VISIONOS AND HALO_RENDERER STREQUAL "BOTH")' in cmake
    gl_block = cmake.split('if(NOT HALO_VISIONOS AND HALO_RENDERER STREQUAL "BOTH")', 1)[1].split('\nendif()', 1)[0]
    assert 'gpu_gl.c' in gl_block
    assert 'gl_functions.c' in gl_block
    assert 'build/ios/gl_include' in gl_block
    assert 'OpenGLES' in gl_block
    assert 'include("${CMAKE_CURRENT_LIST_DIR}/RendererBackendOptions.cmake")' in cmake
    assert 'if(HALO_MAC AND HALO_RENDERER STREQUAL "BOTH")' in cmake
    assert 'target_compile_definitions(VISR PRIVATE HALO_METAL_ONLY=1)' in cmake


@pytest.mark.parametrize('url', [
    'http://packs.example/p.json', 'https:///p.json', 'https://user@packs.example/p.json',
    'https://user:password@packs.example/p.json', 'https://packs.example/p.json?id=device',
    'https://packs.example/p.json#fragment', 'https://packs.example/p.json?',
    'https://packs.example/p.json#', 'https://packs.example/p.json\n',
    'https://packs.example/p file.json', 'https://packs.example:invalid/p.json',
    'https://packs.example/a;b.json', 'https://packs.example/a,b.json', 'https://packs.example/$(HOME).json',
])
@pytest.mark.parametrize('feed', ['profiles', 'recordings'])
def test_data_pack_feed_requires_https_without_credentials_queries_or_fragments(url, feed):
    with pytest.raises(ValueError, match='feed URL'):
        ios_build.data_pack_definitions('', url if feed == 'profiles' else '', url if feed == 'recordings' else '')


@pytest.mark.parametrize('mac', [False, True])
def test_data_pack_cli_settings_reach_the_cmake_configuration(monkeypatch, tmp_path, mac):
    import sys
    commands = []
    monkeypatch.setattr(ios_build, 'ROOT', tmp_path)
    monkeypatch.setattr(ios_build, 'HEADERS', {})
    monkeypatch.setattr(ios_build.platform, 'system', lambda: 'Darwin')
    monkeypatch.setattr(ios_build.platform, 'machine', lambda: 'arm64')
    monkeypatch.setattr(ios_build, 'run', lambda *command: commands.append([str(part) for part in command]))
    monkeypatch.setattr(ios_build.shutil, 'copyfile', lambda *args: None)
    monkeypatch.setattr(ios_build, 'opengles_framework', lambda *args: tmp_path / 'frameworks')
    monkeypatch.setattr(sys, 'argv', ['ios_build.py', '--mac' if mac else '--unsigned', '--host-pgo=off',
                                    '--data-pack-public-key', '12' * 32,
                                    '--profile-feed-url', 'https://packs.example/profiles.json',
                                    '--recording-feed-url', 'https://packs.example/recordings.json'])
    ios_build.main()
    configure = next(command for command in commands if command[0] == 'cmake' and '-S' in command)
    assert '-DHALO_DATA_PACK_PUBLIC_KEY=' + '12' * 32 in configure
    assert '-DHALO_PROFILE_FEED_URL=https://packs.example/profiles.json' in configure
    assert '-DHALO_RECORDING_FEED_URL=https://packs.example/recordings.json' in configure


@pytest.mark.parametrize('name', ['Info.plist', 'Info-macOS.plist', 'Info-tvOS.plist', 'Info-visionOS.plist'])
def test_data_pack_public_configuration_is_embedded_in_every_app_plist(name):
    import plistlib
    from pathlib import Path
    values = plistlib.loads((Path(__file__).resolve().parents[1] / 'port/ios' / name).read_bytes())
    for key, setting in [('HaloDataPackPublicKey', 'HALO_DATA_PACK_PUBLIC_KEY'),
                         ('HaloProfileFeedURL', 'HALO_PROFILE_FEED_URL'),
                         ('HaloRecordingFeedURL', 'HALO_RECORDING_FEED_URL'),
                         ('HaloDataPackChannel', 'HALO_DATA_PACK_CHANNEL')]:
        assert values[key] == f'$({setting})'


def configure_with(monkeypatch, tmp_path, *arguments):
    import sys
    commands = []
    monkeypatch.setattr(ios_build, 'ROOT', tmp_path)
    monkeypatch.setattr(ios_build, 'HEADERS', {})
    monkeypatch.setattr(ios_build.platform, 'system', lambda: 'Darwin')
    monkeypatch.setattr(ios_build.platform, 'machine', lambda: 'arm64')
    monkeypatch.setattr(ios_build, 'run', lambda *command: commands.append([str(part) for part in command]))
    monkeypatch.setattr(ios_build.shutil, 'copyfile', lambda *args: None)
    monkeypatch.setattr(ios_build, 'opengles_framework', lambda *args: tmp_path / 'frameworks')
    monkeypatch.setattr(sys, 'argv', ['ios_build.py', '--host-pgo=off', *arguments])
    ios_build.main()
    return next(command for command in commands if command[0] == 'cmake' and '-S' in command)


@pytest.mark.parametrize('arguments, channel', [(['--unsigned', '--tester'], 'tester'), (['--unsigned'], 'stable'),
                                                (['--simulator', '--tvos', '--tester'], 'tester')])
def test_device_builds_download_from_the_projects_feeds_by_default(monkeypatch, tmp_path, arguments, channel):
    configure = configure_with(monkeypatch, tmp_path, *arguments)
    assert '-DHALO_DATA_PACK_PUBLIC_KEY=' + ios_build.DATA_PACK_PUBLIC_KEY in configure
    assert f'-DHALO_PROFILE_FEED_URL=https://code.steverice.org/visr/packs/{channel}/profiles' in configure
    assert f'-DHALO_RECORDING_FEED_URL=https://code.steverice.org/visr/packs/{channel}/recordings.json' in configure
    assert f'-DHALO_SHADER_FEED_URL=https://code.steverice.org/visr/packs/{channel}/shaders.json' in configure


@pytest.mark.parametrize('arguments, expected', [
    (['--benchmark-routes', 'off'], '-DHALO_BENCHMARK_ROUTES=OFF'),
    ([], '-DHALO_BENCHMARK_ROUTES=ON'),
])
def test_mac_builds_pass_benchmark_route_setting(monkeypatch, tmp_path, arguments, expected):
    configure = configure_with(monkeypatch, tmp_path, '--mac', *arguments)
    assert expected in configure


@pytest.mark.parametrize('arguments', [['--mac'], ['--unsigned', '--data-packs', 'off']])
def test_the_runner_and_data_packs_off_download_nothing(monkeypatch, tmp_path, arguments):
    configure = configure_with(monkeypatch, tmp_path, *arguments)
    assert {'-DHALO_DATA_PACK_PUBLIC_KEY=', '-DHALO_PROFILE_FEED_URL=', '-DHALO_RECORDING_FEED_URL='} <= set(configure)


def test_data_packs_off_refuses_a_named_feed(monkeypatch, tmp_path):
    with pytest.raises(SystemExit):
        configure_with(monkeypatch, tmp_path, '--unsigned', '--data-packs', 'off',
                       '--profile-feed-url', 'https://packs.example/p.json')


def test_the_projects_key_and_feeds_pass_their_own_checks():
    assert ios_build.data_pack_definitions(ios_build.DATA_PACK_PUBLIC_KEY,
                                           ios_build.DATA_PACK_FEED.format(channel='stable', kind='profiles'),
                                           ios_build.DATA_PACK_FEED.format(channel='stable', kind='recordings'))


def test_extended_virtual_addressing_is_allowed_for_ios_but_refused_for_the_mac_build(monkeypatch, capsys, tmp_path):
    arguments = ('--unsigned', '--extended-virtual-addressing')
    with pytest.raises(SystemExit) as failure:
        configure_with(monkeypatch, tmp_path, *arguments, '--mac')
    assert failure.value.code == 2
    assert ios_build.EXTENDED_ADDRESSING_MAC_ERROR in capsys.readouterr().err
    configure = configure_with(monkeypatch, tmp_path, *arguments)
    assert '-DHALO_EXTENDED_VIRTUAL_ADDRESSING=ON' in configure


def test_manual_distribution_signing_never_provisions_or_registers(tmp_path: Path) -> None:
    import plistlib

    commands = ios_build.app_store_commands(tmp_path / 'build', 'iOS', 'TEAM', tmp_path / 'out',
                                           False, True, 'PROFILE-UUID', 'example.app')
    archive, export = commands
    assert 'CODE_SIGN_STYLE=Manual' in archive
    assert 'PROVISIONING_PROFILE_SPECIFIER=PROFILE-UUID' in archive
    assert '-allowProvisioningUpdates' not in archive + export
    assert '-allowProvisioningDeviceRegistration' not in archive + export
    options = plistlib.loads((tmp_path / 'out/ExportOptions.plist').read_bytes())
    assert options['signingStyle'] == 'manual'
    assert options['provisioningProfiles'] == {'example.app': 'PROFILE-UUID'}
    assert options['testFlightInternalTestingOnly'] is True
    assert options['manageAppVersionAndBuildNumber'] is False


def test_manual_signing_requires_bundle_identifier() -> None:
    with pytest.raises(ValueError, match='bundle identifier'):
        ios_build.app_store_options('TEAM', False, profile='PROFILE-UUID')


def test_unsigned_release_probe_can_compile_upload_without_cloud_saves(monkeypatch, tmp_path):
    configure = configure_with(monkeypatch, tmp_path, "--unsigned", "--benchmark-upload")
    assert "-DHALO_CLOUD_SAVES=OFF" in configure
    assert "-DHALO_BENCHMARK_UPLOAD=ON" in configure
    assert "-DHALO_BENCHMARK_APS_ENVIRONMENT=development" in configure


def test_manual_upload_package_uses_production_from_the_installed_profile(monkeypatch):
    from tools import release_signing

    profile = {
        "Entitlements": {
            "com.apple.developer.kernel.extended-virtual-addressing": True,
            "com.apple.developer.icloud-services": ["CloudKit"],
            "com.apple.developer.icloud-container-identifiers": ["iCloud.org.steverice.visr"],
            "aps-environment": "production",
        }
    }
    monkeypatch.setattr(release_signing, "installed_profile", lambda _: profile)
    assert ios_build.benchmark_upload_aps_environment(
        True, True, "PROFILE", "ios", "iCloud.org.steverice.visr"
    ) == "production"


def test_manual_upload_package_without_profile_is_refused():
    with pytest.raises(ValueError, match="existing --signing-profile"):
        ios_build.benchmark_upload_aps_environment(
            True, True, None, "ios", "iCloud.org.steverice.visr"
        )


@pytest.mark.parametrize('name', ['Info.plist', 'Info-macOS.plist', 'Info-tvOS.plist', 'Info-visionOS.plist'])
def test_every_apple_app_declares_exempt_encryption(name):
    import plistlib
    values = plistlib.loads((ios_build.ROOT / 'port/ios' / name).read_bytes())
    assert values.get('ITSAppUsesNonExemptEncryption') is False
    assert 'ITSEncryptionExportComplianceCode' not in values


def test_native_mac_downloads_the_stable_feeds_by_default(monkeypatch, tmp_path):
    configure = configure_with(monkeypatch, tmp_path, '--native-mac')
    assert '-DHALO_DATA_PACK_PUBLIC_KEY=' + ios_build.DATA_PACK_PUBLIC_KEY in configure
    assert '-DHALO_PROFILE_FEED_URL=https://code.steverice.org/visr/packs/stable/profiles' in configure
    assert '-DHALO_SHADER_FEED_URL=https://code.steverice.org/visr/packs/stable/shaders.json' in configure
    assert '-DHALO_DATA_PACK_CHANNEL_NAME=' in configure
    assert '-DHALO_BUNDLE_IDENTIFIER=org.steverice.visr.native' in configure


def test_native_mac_testflight_build_names_the_tester_channel_and_the_ios_record(monkeypatch, tmp_path):
    configure = configure_with(monkeypatch, tmp_path, '--native-mac', '--team', 'TEAM', '--signing-profile', 'UUID',
                               '--data-pack-channel', 'tester')
    assert '-DHALO_DATA_PACK_CHANNEL_NAME=tester' in configure
    assert '-DHALO_PROFILE_FEED_URL=https://code.steverice.org/visr/packs/tester/profiles' in configure
    assert '-DHALO_BUNDLE_IDENTIFIER=org.steverice.visr' in configure
    assert '-DHALO_TESTER_BUILD=OFF' in configure


def test_sandbox_is_only_for_the_native_mac(monkeypatch, tmp_path):
    with pytest.raises(SystemExit):
        configure_with(monkeypatch, tmp_path, '--unsigned', '--sandbox', 'on')


def test_development_hooks_are_compiled_out_of_app_store_packages(tmp_path):
    from types import SimpleNamespace

    assert ios_build.development_hooks(SimpleNamespace(app_store_package=tmp_path)) == '-DHALO_DEVELOPMENT_HOOKS=OFF'
    assert ios_build.development_hooks(SimpleNamespace(app_store_package=None)) == '-DHALO_DEVELOPMENT_HOOKS=ON'
    assert ios_build.development_hooks(SimpleNamespace()) == '-DHALO_DEVELOPMENT_HOOKS=ON'


@pytest.mark.parametrize('path, switch', [
    ('port/ios/host/host_texture_upscale.m', 'HALO_TEXTURE_UPSCALE_STOP_TEST'),
    ('port/ios/host/host_texture_upscale.m', 'HALO_TEXTURE_UPSCALE_CLEAN'),
    ('port/ios/host/texture_upscale_model.m', 'HALO_TEXTURE_UPSCALE_MODEL'),
    ('port/ios/host/host_import.m', 'HALO_TEXTURE_UPSCALE_IMPORT_PHASE'),
])
def test_development_switches_are_read_only_inside_the_hooks_guard(path, switch):
    lines = (Path(__file__).resolve().parents[1] / path).read_text().splitlines()
    # the stack of open conditionals: True for "#ifdef HALO_DEVELOPMENT_HOOKS" (until its #else), False otherwise
    stack, guarded = [], []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith('#if'):
            stack.append(stripped == '#ifdef HALO_DEVELOPMENT_HOOKS')
        elif stripped.startswith(('#else', '#elif')) and stack:
            stack[-1] = False
        elif stripped.startswith('#endif') and stack:
            stack.pop()
        if f'getenv("{switch}")' in line:
            guarded.append(any(stack))
    assert guarded and all(guarded)


def test_the_guard_test_refuses_a_switch_under_another_conditional(tmp_path):
    source = '#if TARGET_OS_IOS\ngetenv("X");\n#endif\n'
    stack, guarded = [], []
    for stripped in source.splitlines():
        if stripped.startswith('#if'):
            stack.append(stripped == '#ifdef HALO_DEVELOPMENT_HOOKS')
        elif stripped.startswith('#endif') and stack:
            stack.pop()
        if 'getenv("X")' in stripped:
            guarded.append(any(stack))
    assert guarded == [False]
