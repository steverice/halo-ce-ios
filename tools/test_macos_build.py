"""Native macOS command contract: separate SDK and local-only development product."""
from types import SimpleNamespace
from pathlib import Path
import pytest
from tools import macos_build


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
