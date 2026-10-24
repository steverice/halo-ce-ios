"""Native macOS command contract: separate SDK and local ad hoc development product."""
import builtins
import subprocess
from types import SimpleNamespace
from pathlib import Path
import pytest
from tools import macos_build
from tools import ios_build


def options(**changes):
    values = dict(bundle_id='org.steverice.visr.native', render_height=0, bink='on',
                  frame_pacing='on', render_validation='off', host_pgo='off', build_number='123', marketing_version=None,
                  renderer='METAL', cloud_saves=False, benchmark_upload=False,
                  team=None, register_device=None, app_store_package=None, upload=False,
                  tester=False, simulator=False, ipa=None, signing_profile=None, sandbox=None,
                  internal_only=False, archive_only=False, data_pack_channel=None)
    values.update(changes)
    return SimpleNamespace(**values)


def test_native_uses_macos_sdk_and_separate_product():
    configure, build = macos_build.native_commands(options(), ['-DHALO_DATA_PACK_PUBLIC_KEY='])
    assert '-DCMAKE_SYSTEM_NAME=Darwin' in configure
    assert '-DCMAKE_OSX_SYSROOT=macosx' in configure
    assert '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0' in configure
    assert '-DHALO_RENDERER=METAL' in configure
    assert '-DHALO_BENCHMARK_ROUTES=OFF' in configure
    assert str(macos_build.ROOT / 'port/macos') in configure
    assert str(macos_build.ROOT / 'build/macos/app-native') in configure
    assert '-DHALO_DEVELOPMENT_TEAM=' in configure
    assert 'CODE_SIGNING_ALLOWED=NO' in build
    assert not any('MACCATALYST' in value or 'Provisioning' in value for value in configure + build)


def test_native_local_routes_flag_is_off_by_default_and_opt_in():
    assert '-DHALO_BENCHMARK_LOCAL_ROUTES=ON' not in macos_build.native_commands(options(), [])[0]
    assert '-DHALO_BENCHMARK_LOCAL_ROUTES=ON' in macos_build.native_commands(options(local_routes=True), [])[0]


@pytest.mark.parametrize('change', [dict(renderer='BOTH'), dict(cloud_saves=True),
    dict(benchmark_upload=True), dict(register_device='device'),
    dict(app_store_package=Path('out')), dict(upload=True), dict(tester=True),
    dict(simulator=True), dict(ipa=Path('out.ipa')), dict(signing_profile='profile'),
    dict(extended_virtual_addressing=True), dict(team='TEAM', sandbox='off'),
    dict(team='TEAM', upload=True, app_store_package=Path('out')), dict(team='TEAM', local_routes=True)])
def test_native_rejects_other_platform_and_account_flows(change):
    with pytest.raises(ValueError):
        macos_build.validate_native(options(**change))


def test_build_native_signs_and_verifies_bundle_before_reporting_success(monkeypatch):
    app = macos_build.ROOT / 'build/macos/app-native/host/Release/VISR.app'
    events = []
    monkeypatch.setattr(macos_build, 'native_commands', lambda args, packs: (['configure'], ['build']))
    monkeypatch.setattr(ios_build, 'run', lambda *command: events.append(('run', command)))
    monkeypatch.setattr(builtins, 'print', lambda *args, **kwargs: events.append(('print', args)))

    macos_build.build_native(options(), [])

    assert events == [
        ('run', ('configure',)),
        ('run', ('build',)),
        ('run', ('codesign', '--force', '--sign', '-', app)),
        ('run', ('codesign', '--verify', '--deep', '--strict', app)),
        ('print', (f'App: {app}',)),
    ]


def test_ad_hoc_sandbox_signs_with_the_sandbox_entitlements_only_when_asked():
    app = macos_build.APP
    assert macos_build.ad_hoc_sign(options()) == ['codesign', '--force', '--sign', '-', app]
    assert macos_build.ad_hoc_sign(options(sandbox='on')) == [
        'codesign', '--force', '--sign', '-', '--entitlements', macos_build.ENTITLEMENTS, app]


def test_team_build_signs_in_xcode_with_the_existing_profile_and_no_provisioning_updates():
    configure, build = macos_build.native_commands(
        options(team='TEAM', bundle_id='org.steverice.visr', signing_profile='UUID'), [])
    assert '-DHALO_DEVELOPMENT_TEAM=TEAM' in configure
    assert '-DHALO_BUNDLE_IDENTIFIER=org.steverice.visr' in configure
    assert 'CODE_SIGNING_ALLOWED=NO' not in build
    assert 'PROVISIONING_PROFILE_SPECIFIER=UUID' in build
    assert '-allowProvisioningUpdates' not in build
    assert not any('DeviceRegistration' in value for value in build)


def test_shipping_bundle_id_is_the_ios_record_and_development_stays_separate():
    assert macos_build.default_bundle_id('TEAM') == 'org.steverice.visr'
    assert macos_build.default_bundle_id(None) == 'org.steverice.visr.native'


def test_team_package_archives_for_macos_and_exports_an_installer_package(monkeypatch, tmp_path):
    import plistlib
    events = []
    monkeypatch.setattr(macos_build, 'native_commands', lambda args, packs: (['configure'], ['build']))
    monkeypatch.setattr(ios_build, 'run', lambda *command: events.append(command))
    monkeypatch.setattr(builtins, 'print', lambda *args, **kwargs: None)
    macos_build.build_native(options(team='TEAM', bundle_id='org.steverice.visr', signing_profile='UUID',
                                     app_store_package=tmp_path, internal_only=True), [])
    app = macos_build.APP
    assert ('codesign', '--force', '--sign', '-', app) not in events
    assert events[2] == ('codesign', '--verify', '--deep', '--strict', app)
    archive, export = events[3], events[4]
    assert archive[:3] == ('xcodebuild', '-project', macos_build.PROJECT)
    assert 'generic/platform=macOS' in archive and archive[-1] == 'archive'
    assert '-allowProvisioningUpdates' not in archive + export
    assert export[:2] == ('xcodebuild', '-exportArchive')
    exported = plistlib.loads((tmp_path / 'ExportOptions.plist').read_bytes())
    assert exported['destination'] == 'export'
    assert exported['provisioningProfiles'] == {'org.steverice.visr': 'UUID'}
    assert exported['installerSigningCertificate'] == ios_build.MAC_INSTALLER_CERTIFICATE
    assert exported['testFlightInternalTestingOnly'] is True


def test_ios_export_options_have_no_installer_certificate():
    options_ = ios_build.app_store_options('TEAM', False, profile='UUID', bundle_id='org.steverice.visr', system='iOS')
    assert 'installerSigningCertificate' not in options_


def test_sandbox_entitlements_cover_import_feeds_and_hosting_without_icloud():
    import plistlib
    entitlements = plistlib.loads(macos_build.ENTITLEMENTS.read_bytes())
    for name in ('com.apple.security.app-sandbox', 'com.apple.security.files.user-selected.read-only',
                 'com.apple.security.files.bookmarks.app-scope', 'com.apple.security.network.client',
                 'com.apple.security.network.server'):
        assert entitlements[name] is True
    # an ad hoc signature cannot carry iCloud keys; team-signed builds add them (port/ios/CMakeLists.txt)
    assert not any(name.startswith('com.apple.developer.') for name in entitlements)
    assert 'com.apple.security.files.user-selected.read-write' not in entitlements


def test_team_signed_cmake_declares_the_shared_icloud_container():
    text = (macos_build.ROOT / 'port/ios/CMakeLists.txt').read_text()
    assert 'HALO_NATIVE_MAC AND HALO_DEVELOPMENT_TEAM' in text
    assert 'iCloud.org.steverice.visr' in text


def test_mac_app_icon_has_every_mac_size():
    import json
    icon = macos_build.ROOT / 'port/macos/Assets-macOS.xcassets/AppIcon.appiconset'
    images = json.loads((icon / 'Contents.json').read_text())['images']
    assert {(image['size'], image['scale']) for image in images if image['idiom'] == 'mac'} == {
        (f'{size}x{size}', f'{scale}x') for size in (16, 32, 128, 256, 512) for scale in (1, 2)}
    assert all((icon / image['filename']).is_file() for image in images)


def test_build_native_does_not_report_success_when_strict_verification_fails(monkeypatch):
    events = []
    app = macos_build.ROOT / 'build/macos/app-native/host/Release/VISR.app'
    monkeypatch.setattr(macos_build, 'native_commands', lambda args, packs: (['configure'], ['build']))

    def run(*command):
        events.append(command)
        if command[:2] == ('codesign', '--verify'):
            raise subprocess.CalledProcessError(1, command)

    monkeypatch.setattr(ios_build, 'run', run)
    monkeypatch.setattr(builtins, 'print', lambda *args, **kwargs: events.append(('print', *args)))

    with pytest.raises(subprocess.CalledProcessError):
        macos_build.build_native(options(), [])

    assert events == [
        ('configure',),
        ('build',),
        ('codesign', '--force', '--sign', '-', app),
        ('codesign', '--verify', '--deep', '--strict', app),
    ]


def test_native_metadata_preserves_shared_signed_download_feed_keys():
    import plistlib

    info = plistlib.loads((macos_build.ROOT / 'port/macos/Info.plist').read_bytes())
    for key, setting in (
        ('HaloDataPackPublicKey', 'HALO_DATA_PACK_PUBLIC_KEY'),
        ('HaloProfileFeedURL', 'HALO_PROFILE_FEED_URL'),
        ('HaloRecordingFeedURL', 'HALO_RECORDING_FEED_URL'),
        ('HaloShaderFeedURL', 'HALO_SHADER_FEED_URL'),
    ):
        assert info[key] == f'$({setting})'


def test_native_metadata_matches_shared_encryption_declaration():
    import plistlib

    info = plistlib.loads((macos_build.ROOT / 'port/macos/Info.plist').read_bytes())
    assert info.get('ITSAppUsesNonExemptEncryption') is False
    assert 'ITSEncryptionExportComplianceCode' not in info
