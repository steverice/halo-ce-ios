#!/usr/bin/env python3
"""Build the separate local macOS/AppKit host around the shared Apple guest."""
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def validate_native(args):
    renderer = args.renderer.upper() if args.renderer else 'METAL'
    if renderer != 'METAL':
        raise ValueError('native macOS supports only --renderer metal')
    forbidden = ('benchmark_upload', 'register_device',
                 'app_store_package', 'upload', 'tester', 'simulator', 'ipa', 'signing_profile',
                 'extended_virtual_addressing')
    if args.cloud_saves and not args.team and not getattr(args, 'unsigned', False):
        raise ValueError('--native-mac with --cloud-saves needs --team')
    if args.team and not args.cloud_saves:
        raise ValueError('--team requires --cloud-saves for native macOS')
    if any(getattr(args, name, None) for name in forbidden):
        raise ValueError('native macOS currently supports local ad hoc development builds only')


def native_commands(args, data_packs):
    validate_native(args)
    if __package__:
        from . import ios_build
    else:
        import ios_build
    build = ROOT / 'build/macos/app-native'
    configure = ['cmake', '-S', str(ROOT / 'port/macos'), '-B', str(build), '-G', 'Xcode',
        '-DCMAKE_SYSTEM_NAME=Darwin', '-DCMAKE_OSX_SYSROOT=macosx', '-DCMAKE_OSX_ARCHITECTURES=arm64',
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0', '-DHALO_RENDERER=METAL',
        f'-DHALO_BUNDLE_IDENTIFIER={args.bundle_id}', f'-DHALO_BINK={args.bink.upper()}',
        f'-DHALO_RENDER_HEIGHT={args.render_height}',
        f'-DHALO_RENDER_VALIDATION={args.render_validation.upper()}',
        *ios_build.cloud_definitions(args.bundle_id, args.cloud_saves, getattr(args, 'cloud_container', None)),
        '-DHALO_BENCHMARK_UPLOAD=OFF', '-DHALO_TESTER_BUILD=OFF', '-DHALO_BENCHMARK_ROUTES=OFF',
        f'-DHALO_BUILD_NUMBER={args.build_number or time.strftime("%Y%m%d.%H%M%S")}',
        *ios_build.host_pgo_flags(args.host_pgo), *data_packs]
    if getattr(args, 'local_routes', False):
        configure.append('-DHALO_BENCHMARK_LOCAL_ROUTES=ON')
    if args.frame_pacing:
        configure.append(f'-DHALO_FRAME_PACING={args.frame_pacing.upper()}')
    if args.marketing_version:
        configure.append(f'-DHALO_MARKETING_VERSION={args.marketing_version}')
    if args.team:
        configure.append(f'-DHALO_DEVELOPMENT_TEAM={args.team}')
    return configure, ['cmake', '--build', str(build), '--config', 'Release', '--target',
                       'VISR', '--', '-quiet',
                       *(['-allowProvisioningUpdates'] if args.cloud_saves and not getattr(args, 'unsigned', False) else ['CODE_SIGNING_ALLOWED=NO'])]


def build_native(args, data_packs):
    if __package__:
        from . import ios_build
    else:
        import ios_build
    configure, build = native_commands(args, data_packs)
    ios_build.run(*configure)
    ios_build.run(*build)
    app = ROOT / 'build/macos/app-native/host/Release/VISR.app'
    if not args.cloud_saves and not getattr(args, 'unsigned', False):
        ios_build.run('codesign', '--force', '--sign', '-', app)
    if not getattr(args, 'unsigned', False):
        ios_build.run('codesign', '--verify', '--deep', '--strict', app)
    print(f'App: {app}')


if __name__ == '__main__':
    if __package__:
        from . import ios_build
    else:
        import ios_build
    sys.argv.insert(1, '--native-mac')
    ios_build.main()
