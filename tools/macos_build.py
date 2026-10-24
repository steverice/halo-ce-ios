#!/usr/bin/env python3
"""Build the separate macOS/AppKit host around the shared Apple guest.

Without --team the app is signed ad hoc for local development (the default); --sandbox on signs that build with the
App Sandbox entitlements too. With --team it is signed for the team and always sandboxed, and --app-store-package
archives and exports it for TestFlight under the iOS app's bundle ID (universal purchase)."""
from pathlib import Path
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/macos/app-native'
PROJECT = BUILD / 'VISRNative.xcodeproj'
APP = BUILD / 'host/Release/VISR.app'
ENTITLEMENTS = ROOT / 'port/macos/VISR-macOS.entitlements'
# the iOS app's record: one App Store Connect record carries every platform (universal purchase)
SHIPPING_BUNDLE_ID = 'org.steverice.visr'
DEVELOPMENT_BUNDLE_ID = 'org.steverice.visr.native'


def default_bundle_id(team):
    return SHIPPING_BUNDLE_ID if team else DEVELOPMENT_BUNDLE_ID


def sandboxed(args):
    """team-signed builds always run in the App Sandbox; ad hoc builds only when asked"""
    return bool(args.team) or getattr(args, 'sandbox', None) == 'on'


def validate_native(args):
    renderer = args.renderer.upper() if args.renderer else 'METAL'
    if renderer != 'METAL':
        raise ValueError('native macOS supports only --renderer metal')
    forbidden = ('cloud_saves', 'benchmark_upload', 'register_device', 'tester', 'simulator', 'ipa',
                 'extended_virtual_addressing')
    if any(getattr(args, name, None) for name in forbidden):
        raise ValueError('native macOS does not support cloud saves, benchmark upload, device registration, tester '
                         'builds, simulators, IPAs or extended virtual addressing')
    if getattr(args, 'upload', False):
        raise ValueError('native macOS exports App Store packages; the CI release helper uploads them')
    if (args.app_store_package or args.signing_profile) and not args.team:
        raise ValueError('--app-store-package and --signing-profile need --team')
    if args.team and getattr(args, 'sandbox', None) == 'off':
        raise ValueError('team-signed native macOS builds always use the App Sandbox')
    if args.team and getattr(args, 'local_routes', False):
        raise ValueError('--local-routes is for ad hoc development builds; a team-signed build runs signed catalogs only')


def native_commands(args, data_packs):
    validate_native(args)
    if __package__:
        from . import ios_build
    else:
        import ios_build
    configure = ['cmake', '-S', str(ROOT / 'port/macos'), '-B', str(BUILD), '-G', 'Xcode',
        '-DCMAKE_SYSTEM_NAME=Darwin', '-DCMAKE_OSX_SYSROOT=macosx', '-DCMAKE_OSX_ARCHITECTURES=arm64',
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0', '-DHALO_RENDERER=METAL',
        f'-DHALO_BUNDLE_IDENTIFIER={args.bundle_id}', f'-DHALO_DEVELOPMENT_TEAM={args.team or ""}',
        f'-DHALO_BINK={args.bink.upper()}', f'-DHALO_RENDER_HEIGHT={args.render_height}',
        f'-DHALO_RENDER_VALIDATION={args.render_validation.upper()}',
        *ios_build.cloud_definitions(args.bundle_id, False, getattr(args, 'cloud_container', None),
                                     shader_upload=getattr(args, 'shader_upload', False)),
        '-DHALO_TESTER_BUILD=OFF', '-DHALO_BENCHMARK_ROUTES=OFF',
        f'-DHALO_BUILD_NUMBER={args.build_number or time.strftime("%Y%m%d.%H%M%S")}',
        f'-DHALO_DATA_PACK_CHANNEL_NAME={getattr(args, "data_pack_channel", None) or ""}',
        *ios_build.host_pgo_flags(args.host_pgo), *data_packs]
    if getattr(args, 'local_routes', False):
        configure.append('-DHALO_BENCHMARK_LOCAL_ROUTES=ON')
    if args.frame_pacing:
        configure.append(f'-DHALO_FRAME_PACING={args.frame_pacing.upper()}')
    if args.marketing_version:
        configure.append(f'-DHALO_MARKETING_VERSION={args.marketing_version}')
    if getattr(args, 'shader_upload', False) and not args.team and not getattr(args, 'unsigned', False):
        raise ValueError('--native-mac --shader-upload needs --team or --unsigned for compile validation')
    build = ['cmake', '--build', str(BUILD), '--config', 'Release', '--target', 'VISR', '--', '-quiet']
    if args.team:
        build.extend(ios_build.manual_signing(args.signing_profile) or ['-allowProvisioningUpdates'])
    else:
        build.append('CODE_SIGNING_ALLOWED=NO')
    return configure, build


def ad_hoc_sign(args):
    """the local development signature, with the sandbox entitlements when asked for"""
    entitlements = ['--entitlements', ENTITLEMENTS] if sandboxed(args) else []
    return ['codesign', '--force', '--sign', '-', *entitlements, APP]


def build_native(args, data_packs):
    if __package__:
        from . import ios_build
    else:
        import ios_build
    configure, build = native_commands(args, data_packs)
    ios_build.run(*configure)
    # an App Store archive leaves the product as a link into its own intermediates (as on iOS)
    if APP.is_symlink():
        APP.unlink()
    ios_build.run(*build)
    if getattr(args, 'unsigned', False):
        print(f'App (unsigned, compile check only): {APP}')
        return
    if not args.team:
        ios_build.run(*ad_hoc_sign(args))
    ios_build.run('codesign', '--verify', '--deep', '--strict', APP)
    print(f'App: {APP}')
    if args.app_store_package:
        package(args, ios_build)


def package(args, ios_build):
    out = args.app_store_package.resolve()
    commands = ios_build.app_store_commands(BUILD, 'macOS', args.team, out, False, args.internal_only,
                                            args.signing_profile, args.bundle_id, project=PROJECT)
    ios_build.run(*commands[0])
    # the archive's dSYMs folder stays empty for this CMake project, as on iOS (tools/ios_build.py)
    dsym = APP.parent / 'VISR.app.dSYM'
    if dsym.is_dir():
        target = out / 'VISR.xcarchive/dSYMs/VISR.app.dSYM'
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(dsym, target, symlinks=True)
        print(f'dSYM: {target}')
    else:
        print(f'no dSYM at {dsym}: the upload will carry no debug symbols', flush=True)
    for command in [] if args.archive_only else commands[1:]:
        ios_build.run(*command)
    print(f'App Store Connect package: {out}' + (' (archive only: not exported)' if args.archive_only else ''))


if __name__ == '__main__':
    if __package__:
        from . import ios_build
    else:
        import ios_build
    sys.argv.insert(1, '--native-mac')
    ios_build.main()
