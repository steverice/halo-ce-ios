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
                  tester=False, simulator=False, ipa=None, signing_profile=None)
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
    assert 'CODE_SIGNING_ALLOWED=NO' in build
    assert not any('MACCATALYST' in value or 'Provisioning' in value for value in configure + build)


@pytest.mark.parametrize('change', [dict(renderer='BOTH'), dict(cloud_saves=True),
    dict(benchmark_upload=True), dict(team='TEAM'), dict(register_device='device'),
    dict(app_store_package=Path('out')), dict(upload=True), dict(tester=True),
    dict(simulator=True), dict(ipa=Path('out.ipa')), dict(signing_profile='profile'),
    dict(extended_virtual_addressing=True)])
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
