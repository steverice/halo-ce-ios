#!/usr/bin/env python3
"""Build the iPhone/iPad (or, with --tvos, Apple TV; with --visionos, Apple Vision Pro) app from source on an Apple Silicon Mac."""
from __future__ import annotations

import argparse
import os
import platform
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request

if __package__:
    from . import release_signing
else:
    import release_signing

ROOT = Path(__file__).resolve().parents[1]
GL_REVISION = '1cdd228e34966dd6b95bd203e9f84faba0f371a1'
EGL_REVISION = 'db3425b8246136faccb5e2782b5694960bd6edf1'
HEADERS = {
    'GLES3/gl32.h': ('OpenGL-Registry', GL_REVISION, 'api/GLES3/gl32.h'),
    'GLES3/gl3platform.h': ('OpenGL-Registry', GL_REVISION, 'api/GLES3/gl3platform.h'),
    'GLES2/gl2platform.h': ('OpenGL-Registry', GL_REVISION, 'api/GLES2/gl2platform.h'),
    'GLES2/gl2ext.h': ('OpenGL-Registry', GL_REVISION, 'api/GLES2/gl2ext.h'),
    'KHR/khrplatform.h': ('EGL-Registry', EGL_REVISION, 'api/KHR/khrplatform.h'),
}

# signed profile and recording updates (docs/data-packs.md): the key the publisher's seed pairs with, and the
# feeds on the project's site, one folder per channel
DATA_PACK_PUBLIC_KEY = 'b57463a3c1b157fdb49a4df5c2041b88cabf07b4ba733d0c7290b90d85e3b5ea'
DATA_PACK_FEED = 'https://code.steverice.org/visr/packs/{channel}/{kind}.json'

HOST_PROFILE = ROOT / 'pgo/visr_host.profdata'
HOST_PROFILE_RAW = ROOT / 'build/host-pgo/raw'


def host_pgo_flags(mode):
    """the CMake settings for --host-pgo: use (the profile in pgo/, when this Xcode's clang can read it),
    generate (an instrumented app that writes raw profiles into build/host-pgo/raw when it exits) or off"""
    if mode == 'generate':
        HOST_PROFILE_RAW.mkdir(parents=True, exist_ok=True)
        return ['-DHALO_HOST_PGO=generate', f'-DHALO_HOST_PROFILE={HOST_PROFILE_RAW}/visr-%p.profraw']
    if mode == 'use':
        if not HOST_PROFILE.is_file():
            print(f'{HOST_PROFILE} is missing: building the host without profile-guided optimization', flush=True)
        elif subprocess.run(['xcrun', 'llvm-profdata', 'show', HOST_PROFILE], capture_output=True).returncode:
            # a profile written by a newer LLVM than this Xcode's cannot be read, and clang would stop on it
            print(f'this Xcode cannot read {HOST_PROFILE} (recorded with a newer one): building the host without '
                  'profile-guided optimization', flush=True)
        else:
            return ['-DHALO_HOST_PGO=use', f'-DHALO_HOST_PROFILE={HOST_PROFILE}']
    return ['-DHALO_HOST_PGO=off', '-DHALO_HOST_PROFILE=']


def drop_stale_ffmpeg_license(build, bink):
    """a build folder that last built an app with FFmpeg still holds its license in the app bundle, which
    Xcode does not remove when the file leaves the project: remove it before an app without FFmpeg is built"""
    if bink == 'off':
        for stale in Path(build).glob('*/VISR.app/**/FFmpeg-LGPL-2.1.txt'):
            stale.unlink()


def run(*args):
    print('+', ' '.join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


EXTENDED_ADDRESSING_MAC_ERROR = '--extended-virtual-addressing is for device builds, not --mac'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--simulator', action='store_true', help='build for an ARM64 simulator')
    mode.add_argument('--unsigned', action='store_true', help='build a device app for signing later')
    parser.add_argument('--team', help='Apple development team ID for device signing')
    parser.add_argument('--bundle-id', help='bundle identifier covered by your signing profile (default org.steverice.visr for iOS, tvOS, visionOS and a team-signed --native-mac, so one App Store Connect record covers them all; org.steverice.visr.mac for --mac; org.steverice.visr.native for an ad hoc --native-mac)')
    target = parser.add_mutually_exclusive_group()
    target.add_argument('--tvos', action='store_true', help='build for Apple TV instead of iPhone/iPad')
    target.add_argument('--visionos', action='store_true', help='build for Apple Vision Pro (Metal only) instead of iPhone/iPad')
    target.add_argument('--native-mac', action='store_true', help='build the separate local macOS AppKit host')
    target.add_argument('--mac', action='store_true', help='build the iOS host for Mac Catalyst, ad hoc signed (the native Mac runner)')
    parser.add_argument('--renderer', choices=('both', 'metal'),
                        help='renderer backends to compile: both (default except visionOS) lets config.toml select OpenGL ES or Metal; '
                             'metal omits OpenGL ES sources, headers and framework')
    parser.add_argument('--render-height', type=int,
                        help='tvOS and visionOS: internal render height in pixels, 0 for native '
                             '(default 1080 on tvOS; native on visionOS, which follows the window\'s size)')
    parser.add_argument('--extended-virtual-addressing', action='store_true',
                        help='sign with the extended virtual addressing entitlement (paid developer '
                             'teams), in case the device refuses the 4 GB guest arena (not for --mac)')
    parser.add_argument('--frame-pacing', choices=('on','off'),
                        help="compile in the Metal renderer's presentation pacing (HALO_FRAME_PACING; "
                             'default on, off for --visionos, whose frames the Compositor paces)')
    parser.add_argument('--register-device', metavar='UDID',
                        help='signed device builds: build for this one device and let Xcode register it with the team '
                             '(-allowProvisioningDeviceRegistration). Off by default: a build never registers a device unasked')
    parser.add_argument('--cloud-saves', action='store_true',
                        help='sync saves through the player\'s private iCloud database (docs/cloud-saves.md). The App ID '
                             'needs the iCloud capability with CloudKit and the container; a signed build with this '
                             'option lets Xcode add them to your developer account unless --signing-profile selects an existing profile')
    parser.add_argument('--cloud-container',
                        help='--cloud-saves: the iCloud container (default iCloud.org.steverice.visr for the '
                             'org.steverice.visr bundle IDs; required with any other bundle ID)')
    parser.add_argument('--perf-reports', action='store_true',
                        help='collect and deliver game performance reports through CloudKit')
    parser.add_argument('--shader-upload', action='store_true',
                        help='contribute missing Metal shader descriptions through CloudKit')
    parser.add_argument('--benchmark-upload', action='store_true',
                        help='let the device benchmark automatically send its report to the public database '
                             'of the iCloud container (--cloud-container, or the project\'s own). Signs with the iCloud '
                             'entitlement as --cloud-saves does, without turning cloud saves on')
    parser.add_argument('--bink', choices=('on', 'off'), default='on',
                        help='play the intro and credits movies with the vendored FFmpeg Bink decoders '
                             '(port/third_party/ffmpeg-bink, LGPL); off builds an app with no FFmpeg code, '
                             'which skips the movies (default on)')
    parser.add_argument('--ipa', type=Path, help='also package the device app at this path')
    parser.add_argument('--tester', action='store_true',
                        help='a tester build: Play and Benchmark at launch, and the privacy manifest that declares '
                             'the benchmark report (port/ios/privacy)')
    parser.add_argument('--benchmark-routes', choices=('on','off'),
                        help="bundle the device benchmark's recorded routes and run them as scenes (default on; off "
                             'for --tester, whose suite has only the scenes that play by themselves until recorded '
                             'routes are shown to hold on other devices)')
    parser.add_argument('--local-routes', action='store_true',
                        help='native Mac development builds: run the unsigned benchmark/local-routes catalog '
                             '(tools/replay_check.py, step-tools replay review) without bundling recordings')
    parser.add_argument('--archive-only', action='store_true',
                        help='with --app-store-package: stop after the archive. The export that follows is what '
                             'makes the distribution certificate and App Store profile')
    parser.add_argument('--app-store-package', type=Path, metavar='FOLDER',
                        help='signed device builds: also archive the app and export an App Store Connect package '
                             '(for TestFlight) into this folder. Xcode makes the team\'s distribution certificate and '
                             'App Store profile if they are missing, which changes the developer account')
    parser.add_argument('--marketing-version', help='release version supplied by the CI release tag')
    parser.add_argument('--build-number', help='CFBundleVersion supplied by the CI release manifest')
    parser.add_argument('--signing-profile', help='existing distribution profile UUID; use manual signing without provisioning updates')
    parser.add_argument('--upload', action='store_true',
                        help='with --app-store-package: upload the package to App Store Connect with the account '
                             'Xcode is signed in to. The app record must exist there first')
    parser.add_argument('--data-packs', choices=('on', 'off'),
                        help='signed profile and recording updates (docs/data-packs.md): on by default for device and '
                             'simulator builds and --native-mac, from the tester feeds with --tester and the stable feeds '
                             'otherwise; off by default for --mac, the runner, whose runs must not change with a download, '
                             'unless a key or feed is named')
    parser.add_argument('--data-pack-channel', choices=('stable', 'tester'),
                        help='the channel the signed updates come from (default tester with --tester, stable '
                             'otherwise); the native Mac app has no tester build, so its TestFlight build names tester')
    parser.add_argument('--sandbox', choices=('on', 'off'),
                        help='--native-mac: sign the ad hoc development build with the App Sandbox entitlements (off by '
                             'default). Team-signed native builds are always sandboxed')
    parser.add_argument('--data-pack-public-key', metavar='HEX',
                        help='the Ed25519 public key (64 hex digits) that updates must verify against, in place of the '
                             "project's own")
    parser.add_argument('--profile-feed-url', metavar='URL',
                        help='the HTTPS address of the signed profile update envelope, in place of the default')
    parser.add_argument('--shader-feed-url', metavar='URL', help='the HTTPS signed additive shader-list envelope')
    parser.add_argument('--recording-feed-url', metavar='URL',
                        help='the HTTPS address of the signed benchmark recording update envelope, in place of the default')
    parser.add_argument('--internal-only', action='store_true',
                        help='with --app-store-package: restrict the exported build to internal TestFlight groups')
    parser.add_argument('--collision-log', action='store_true',
                        help='time and count the collision functions for the on-screen collision log (a debug and profiling '
                             'aid); without it the clock reads and counters are compiled out')
    parser.add_argument('--profile', action='store_true',
                        help='a profiling build: the game records CPU scopes into trace parts in the data folder\'s '
                             'profiles folder (debug.profile_record, or the profile_record console command; '
                             'docs/profiling.md); without it the recorder is compiled out')
    parser.add_argument('--render-validation', choices=('on', 'off'),
                        default='on' if os.environ.get('HALO_RENDER_VALIDATION_BUILD') == '1' else 'off',
                        help='check every call into the GPU backend before it is made, and name every fallback '
                             '(debug.render_validation: off, log or strict; log unless set). Off by default: a build '
                             'without it has no trace of it. For test builds, never for a release or a benchmark. '
                             'HALO_RENDER_VALIDATION_BUILD=1 in the environment turns it on for tools that build for you')
    parser.add_argument('--pgo', choices=('use', 'off'), default='use',
                        help='profile-guided optimization of the game code: use (the default) builds it with the '
                             'profile in pgo/, off builds it without')
    parser.add_argument('--lto', choices=('full', 'off'), default='full',
                        help='link-time optimization of the game code: full (the default) optimizes the game, the '
                             'platform layer and the bundled libraries as one module, off compiles each file alone')
    parser.add_argument('--host-pgo', choices=('use', 'off', 'generate'), default='use',
                        help='profile-guided optimization of the host (the app, the Metal backend, SDL): use (the '
                             'default) builds with pgo/visr_host.profdata, off without, generate builds the '
                             'instrumented app that tools/host_pgo_train.py records the profile with')
    parser.add_argument('--llvm', default='/opt/homebrew/opt/llvm')
    parser.add_argument('--lld', default='/opt/homebrew/opt/lld/bin/ld.lld')
    parser.add_argument('--jobs', type=int, default=min(12,os.cpu_count() or 4))
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('an Apple Silicon Mac with full Xcode is required')
    if not args.simulator and not args.unsigned and not args.team and not args.mac and not args.native_mac:
        parser.error('use --team YOUR_TEAM_ID to sign, or --unsigned to sign later')
    if args.team and (args.unsigned or args.simulator):
        parser.error('--team is only used for signed device builds')
    if args.register_device and not args.team:
        parser.error('--register-device is for signed device builds (--team)')
    if args.ipa and args.simulator:
        parser.error('--ipa requires a device build')
    if args.app_store_package and not args.team:
        parser.error('--app-store-package is for signed device builds (--team)')
    if args.marketing_version and not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', args.marketing_version):
        parser.error('--marketing-version must have three numeric components')
    if args.build_number and not re.fullmatch(r'[0-9]+(?:\.[0-9]+){0,2}', args.build_number):
        parser.error('--build-number must have one to three numeric components')
    if args.signing_profile and (not args.team or args.register_device):
        parser.error('--signing-profile requires --team and cannot change account capabilities or register devices')
    if args.signing_profile and args.upload:
        parser.error('--signing-profile archives/exports only; the CI release helper uploads with API authentication')
    if args.profile and args.app_store_package:
        parser.error('--profile is for development builds, never an App Store package')
    if args.upload and not args.app_store_package:
        parser.error('--upload needs --app-store-package')
    if args.internal_only and not args.app_store_package:
        parser.error('--internal-only needs --app-store-package')
    if args.archive_only and (not args.app_store_package or args.upload):
        parser.error('--archive-only goes with --app-store-package and without --upload')
    if args.tester and args.mac:
        parser.error('--tester is for device and simulator builds')
    if (args.shader_upload or args.perf_reports) and args.app_store_package and not args.signing_profile:
        parser.error('--shader-upload/--perf-reports App Store packages require an existing --signing-profile')
    if args.benchmark_upload and args.simulator:
        parser.error('--benchmark-upload needs a device build')
    if args.benchmark_upload and args.app_store_package and not args.signing_profile:
        parser.error('--benchmark-upload App Store packages require an existing --signing-profile')
    if args.render_height is None:
        # 0: the device profile's (profiles/); a number here overrides every profile
        args.render_height = 0
    if args.extended_virtual_addressing and args.mac:
        parser.error(EXTENDED_ADDRESSING_MAC_ERROR)
    platform_name = 'tvos' if args.tvos else 'visionos' if args.visionos else 'mac' if args.mac else 'ios'
    renderer = args.renderer.upper() if args.renderer else ('METAL' if args.visionos or args.native_mac else 'BOTH')
    if args.visionos and renderer != 'METAL':
        parser.error('--visionos supports only --renderer metal')
    if not args.native_mac:
        args.bundle_id = args.bundle_id or ('org.steverice.visr.mac' if platform_name == 'mac' else 'org.steverice.visr')
    if args.sandbox and not args.native_mac:
        parser.error('--sandbox is for --native-mac')
    if args.native_mac:
        if __package__:
            from .macos_build import default_bundle_id, validate_native
        else:
            from macos_build import default_bundle_id, validate_native
        args.bundle_id = args.bundle_id or default_bundle_id(args.team)
        try:
            validate_native(args)
        except ValueError as error:
            parser.error(str(error))
    if args.mac and (args.simulator or args.unsigned or (args.team and not (args.cloud_saves or args.benchmark_upload or args.shader_upload or args.perf_reports))):
        parser.error('--mac builds are ad hoc signed; drop --simulator, --unsigned and --team')
    try:
        cloud = cloud_definitions(args.bundle_id, args.cloud_saves, args.cloud_container, args.benchmark_upload, args.shader_upload, args.perf_reports)
    except ValueError as error:
        parser.error(str(error))
    cloud_container = next(
        (
            item.split("=", 1)[1]
            for item in cloud
            if item.startswith("-DHALO_CLOUD_CONTAINER=")
        ),
        "iCloud.org.steverice.visr",
    )
    try:
        benchmark_aps = benchmark_upload_aps_environment(
            args.benchmark_upload or args.shader_upload or args.perf_reports or args.cloud_saves,
            bool(args.app_store_package),
            args.signing_profile,
            'macos' if args.native_mac else platform_name,
            cloud_container,
        )
    except ValueError as error:
        parser.error(str(error))
    try:
        data_packs = data_pack_definitions(*data_pack_settings(args))
    except ValueError as error:
        parser.error(str(error))
    if (args.mac or args.native_mac) and (args.cloud_saves or args.benchmark_upload or args.shader_upload or args.perf_reports) and not args.team and not (args.native_mac and args.unsigned):
        parser.error('--mac with CloudKit saves or uploads needs --team: macOS does not run an ad hoc signed '
                     'app with iCloud entitlements')
    if args.cloud_saves and args.team and not args.signing_profile:
        print(f'iCloud saves: this signed build lets Xcode enable the iCloud capability and the container '
              f'{cloud[1].split("=", 1)[1]} on the App ID {args.bundle_id} and make a new profile', flush=True)
    if not re.fullmatch(r'[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', args.bundle_id):
        parser.error('--bundle-id must be a reverse-DNS identifier (e.g. com.example.halo)')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if renderer == 'BOTH':
        include=ROOT/'build/ios/gl_include'
        for name,(registry,revision,source) in HEADERS.items():
            target=include/name;target.parent.mkdir(parents=True,exist_ok=True)
            url=f'https://raw.githubusercontent.com/KhronosGroup/{registry}/{revision}/{source}'
            data=urllib.request.urlopen(url,timeout=30).read()
            if not target.exists() or target.read_bytes()!=data:target.write_bytes(data)
    validation = args.render_validation == 'on'
    run(sys.executable,'configure.py','--ios',f'--pgo={args.pgo}',f'--lto={args.lto}','--ios-llvm',args.llvm,'--ios-lld',args.lld,
        *(['--render-validation'] if validation else []),*(['--collision-log'] if args.collision_log else []),
        *(['--profile'] if args.profile else []))
    run('ninja','ios_guest','-j',args.jobs)
    run(sys.executable,'tools/ios_bridges.py',*(['--render-validation'] if validation else []))
    # what a render capture is checked against when it is replayed (port/ios/host/gpu_capture.h)
    if validation: run(sys.executable,'tools/gpu_interface_hash.py')
    run(sys.executable,'tools/ios_embed_guest.py','build/ios/halo_guest.elf','build/ios/embedded')
    notices = ROOT/'build/ios/licenses'
    notices.mkdir(parents=True, exist_ok=True)
    for source, name in (
        ('LICENSE', 'Project-Apache-2.0.txt'),
        ('LICENSES/CC0-1.0.txt', 'Origin-Projects-CC0.txt'),
        ('NOTICE.md', 'NOTICE-VISR.txt'),
        ('port/ios/THIRD_PARTY.md', 'NOTICE.txt'),
        ('build/third_party/SDL3/LICENSE.txt', 'SDL.txt'),
        ('build/third_party/musl-1.2.5/COPYRIGHT', 'musl.txt'),
        ('port/third_party/kcp/LICENSE', 'kcp.txt'),
        ('port/third_party/tomlc17/LICENSE', 'tomlc17.txt'),
        ('port/third_party/stb/LICENSE', 'stb_truetype.txt'),
        ('port/third_party/expat/COPYING', 'expat.txt'),
        ('port/third_party/monocypher/LICENCE.md', 'Monocypher.txt'),
        ('port/third_party/mbedtls/LICENSE', 'MbedTLS-Apache-2.0.txt'),
        ('port/third_party/zlib/LICENSE', 'zlib.txt'),
        ('port/third_party/astcenc/LICENSE.txt', 'astc-encoder.txt'),
        ('port/third_party/xxhash/LICENSE', 'xxHash-BSD-2-Clause.txt'),
        ('port/assets/fonts/Overpass-OFL.txt', 'Overpass-OFL.txt'),
        ('port/assets/text_filter/README.md', 'Streamer-Words-MIT.txt'),
        ('port/third_party/english-words/LICENSE.md', 'English-Words-Unlicense.txt'),
        ('port/assets/fonts/OpenCE-OFL.txt', 'OpenCE-OFL.txt'),
        ('port/assets/fonts/Newtown-LICENSE.txt', 'Newtown.txt'),
        ('port/third_party/smaa/LICENSE', 'SMAA.txt'),
    ):
        shutil.copyfile(ROOT/source, notices/name)
    # FFmpeg's license goes into the apps that contain FFmpeg, and no others
    ffmpeg_license = notices/'FFmpeg-LGPL-2.1.txt'
    if args.bink == 'on':
        shutil.copyfile(ROOT/'port/third_party/ffmpeg-bink/COPYING.LGPLv2.1', ffmpeg_license)
    else:
        ffmpeg_license.unlink(missing_ok=True)
    if args.native_mac:
        if __package__:
            from .macos_build import build_native
        else:
            from macos_build import build_native
        build_native(args, data_packs)
        return
    if args.mac:
        build_mac(args, cloud, data_packs)
        return
    build=ROOT/'build'/platform_name/('app-simulator' if args.simulator else
                'app-unsigned' if args.unsigned else 'app-device')
    if args.tvos: sdk='appletvsimulator' if args.simulator else 'appletvos'
    elif args.visionos: sdk='xrsimulator' if args.simulator else 'xros'
    else: sdk='iphonesimulator' if args.simulator else 'iphoneos'
    # visionOS 2.0 is the first with MTLCompileOptions.mathMode (gpu_metal.m)
    system,deployment={'tvos':('tvOS','16.0'),'visionos':('visionOS','2.0'),'ios':('iOS','16.0')}[platform_name]
    command=['cmake','-S','port/ios','-B',build,'-G','Xcode',f'-DCMAKE_SYSTEM_NAME={system}',
             f'-DCMAKE_OSX_SYSROOT={sdk}','-DCMAKE_OSX_ARCHITECTURES=arm64',f'-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment}',
             f'-DHALO_BUNDLE_IDENTIFIER={args.bundle_id}', f'-DHALO_DEVELOPMENT_TEAM={args.team or ""}',
             f'-DHALO_RENDER_HEIGHT={args.render_height}', f'-DHALO_RENDERER={renderer}', f'-DHALO_BINK={args.bink.upper()}',
             f'-DHALO_EXTENDED_VIRTUAL_ADDRESSING={"ON" if args.extended_virtual_addressing else "OFF"}', *cloud, *data_packs,
             f'-DHALO_DATA_PACK_CHANNEL_NAME={args.data_pack_channel or ""}',
             f'-DHALO_RENDER_VALIDATION={"ON" if validation else "OFF"}',
             *host_pgo_flags(args.host_pgo)]
    if args.benchmark_upload or args.shader_upload or args.perf_reports or args.cloud_saves:
        command.append(f'-DHALO_BENCHMARK_APS_ENVIRONMENT={benchmark_aps}')
    if args.frame_pacing:
        command.append(f'-DHALO_FRAME_PACING={args.frame_pacing.upper()}')
    command.append(f'-DHALO_TESTER_BUILD={"ON" if args.tester else "OFF"}')
    routes = args.benchmark_routes or ('off' if args.tester else 'on')
    command.append(f'-DHALO_BENCHMARK_ROUTES={routes.upper()}')
    # a new CFBundleVersion each build, so an install over the app replaces it (on an Apple TV that keeps
    # the imported maps: an uninstall would take them, and the upload with them)
    import time
    command.append(f'-DHALO_BUILD_NUMBER={args.build_number or time.strftime("%Y%m%d.%H%M%S")}')
    if args.marketing_version:
        command.append(f'-DHALO_MARKETING_VERSION={args.marketing_version}')
    run(*command)
    # an App Store archive leaves the product as a link into its own intermediates, which the next plain build
    # cannot write through ("unable to create directory"): a link there is removed first
    product = build/f'Release-{sdk}/VISR.app'
    if product.is_symlink():
        product.unlink()
    drop_stale_ffmpeg_license(build, args.bink)
    command=['cmake','--build',build,'--config','Release','--target','VISR','--','-quiet']
    if args.simulator or args.unsigned:command.append('CODE_SIGNING_ALLOWED=NO')
    else:
        command.extend(manual_signing(args.signing_profile) or ['-allowProvisioningUpdates'])
        if args.register_device:
            # xcodebuild ignores a destination without a scheme, so this build names the scheme itself
            # (CMake's Xcode generator makes one per target) and keeps the products where the other builds put them
            command=['xcodebuild','-project',build/'VISR.xcodeproj','-scheme','VISR','-configuration','Release',
                     '-destination',f'platform={system},id={args.register_device}','-quiet',
                     '-allowProvisioningUpdates','-allowProvisioningDeviceRegistration',f'SYMROOT={build}','build']
    run(*command)
    app = build/f'Release-{sdk}/VISR.app'
    print(f'App: {app}')
    if args.ipa:
        command = [sys.executable, 'tools/ios_package.py', app, args.ipa.resolve()]
        if args.unsigned: command.append('--require-unsigned')
        run(*command)
    if args.app_store_package:
        commands = app_store_commands(build, system, args.team, args.app_store_package.resolve(), args.upload,
                                      args.internal_only, args.signing_profile, args.bundle_id)
        run(*commands[0])
        # the archive's own dSYMs folder stays empty for this CMake project (its products go to the build folder,
        # where the archive does not look), so the dSYM the archive build wrote there goes in by hand; the UUIDs
        # match, since it is the same build
        dsym = build/f'Release-{sdk}/VISR.app.dSYM'
        if dsym.is_dir():
            target = args.app_store_package.resolve()/'VISR.xcarchive/dSYMs/VISR.app.dSYM'
            shutil.rmtree(target, ignore_errors=True)
            shutil.copytree(dsym, target, symlinks=True)
            print(f'dSYM: {target}')
        else:
            print(f'no dSYM at {dsym}: the upload will carry no debug symbols', flush=True)
        # the guest's symbols, which name the guest frames of this build's crash reports
        # (tools/guest_symbolicate.py); the app carries the same table in __TEXT,__halosyms
        symbols = ROOT/'build/ios/embedded/guest-symbols.txt'
        shutil.copy2(symbols, args.app_store_package.resolve()/'guest-symbols.txt')
        print(f'guest symbols: {args.app_store_package.resolve()/"guest-symbols.txt"}')
        for command in [] if args.archive_only else commands[1:]:
            run(*command)
        print(f'App Store Connect package: {args.app_store_package}' +
              (' (archive only: not exported)' if args.archive_only else ' (uploaded)' if args.upload else ''))


def manual_signing(profile: str | None) -> list[str]:
    return ['CODE_SIGN_STYLE=Manual', 'CODE_SIGN_IDENTITY=Apple Distribution',
            f'PROVISIONING_PROFILE_SPECIFIER={profile}'] if profile else []


# the certificate that signs a Mac App Store installer package ("Mac Installer Distribution" in the developer account)
MAC_INSTALLER_CERTIFICATE = '3rd Party Mac Developer Installer'


def app_store_options(team: str, upload: bool, internal_only: bool = False,
                      profile: str | None = None, bundle_id: str | None = None,
                      system: str | None = None) -> dict[str, object]:
    """the export options for an App Store Connect package: signed for distribution by Xcode's automatic signing,
    and either written to disk or uploaded"""
    options: dict[str, object] = {'method': 'app-store-connect', 'destination': 'upload' if upload else 'export', 'teamID': team,
               'signingStyle': 'automatic', 'manageAppVersionAndBuildNumber': False, 'uploadSymbols': True}
    if profile:
        if not bundle_id:
            raise ValueError('manual signing needs the actual bundle identifier')
        options.update(signingStyle='manual', signingCertificate='Apple Distribution',
                       provisioningProfiles={bundle_id: profile})
        if system == 'macOS':
            # a Mac app goes to App Store Connect as a signed installer package, not an IPA
            options['installerSigningCertificate'] = MAC_INSTALLER_CERTIFICATE
    if internal_only:
        options['testFlightInternalTestingOnly'] = True
    return options


def app_store_commands(build: Path, system: str, team: str, out: Path, upload: bool, internal_only: bool = False,
                       profile: str | None = None, bundle_id: str | None = None,
                       project: Path | None = None) -> list[list[str | Path]]:
    """the commands that archive the configured project and export (or upload) its App Store Connect package into
    out; writes out/ExportOptions.plist for the second"""
    import plistlib
    out.mkdir(parents=True, exist_ok=True)
    (out/'ExportOptions.plist').write_bytes(plistlib.dumps(app_store_options(team, upload, internal_only, profile, bundle_id, system)))
    archive = out/'VISR.xcarchive'
    return [['xcodebuild', '-project', project or build/'VISR.xcodeproj', '-scheme', 'VISR', '-configuration', 'Release',
             '-destination', f'generic/platform={system}', '-archivePath', archive, '-quiet',
             *(manual_signing(profile) or ['-allowProvisioningUpdates']), 'archive'],
            ['xcodebuild', '-exportArchive', '-archivePath', archive, '-exportOptionsPlist', out/'ExportOptions.plist',
             '-exportPath', out, *([] if profile else ['-allowProvisioningUpdates'])]]

# Known iPhoneOS SDK target sets. Export lists must match the full set.
IOS_STUB_TARGETS = (
    frozenset(('armv7-ios', 'armv7s-ios', 'arm64-ios', 'arm64e-ios')),
    frozenset(('arm64e-ios', 'arm64e.x1-ios')),
    frozenset(('arm64e-ios',)),
)
CATALYST_STUB_TARGETS = '[ arm64-maccatalyst, arm64e-maccatalyst ]'
IOS_INSTALL_NAME = "'/System/Library/Frameworks/OpenGLES.framework/OpenGLES'"
CATALYST_INSTALL_NAME = "'/System/iOSSupport/System/Library/Frameworks/OpenGLES.framework/OpenGLES'"


def maccatalyst_stub(text):
    """the iPhoneOS SDK's OpenGLES.tbd rewritten to link against the Mac's macCatalyst
    OpenGLES.framework. A stub in any other form (a later Xcode's) is refused, not half rewritten."""
    pattern = re.compile(r'(^[ \t]*(?:-[ \t]*)?targets:\s*)\[([^\]]*)\]', re.MULTILINE)
    lists = [tuple(token.strip().strip("'\"") for token in match[2].split(',') if token.strip())
             for match in pattern.finditer(text)]
    version = re.search(r'["\']?(?:tbd-version|tapi_tbd_version)["\']?\s*:\s*["\']?(\d+)', text)
    # Only metadata belongs in an error: never dump SDK symbols or the complete stub.
    reported = list(dict.fromkeys('[' + ', '.join(targets) + ']' for targets in lists))
    if not reported:
        reported = list(dict.fromkeys(re.findall(r'"target"\s*:\s*"([^"\n]+)"', text)))
    metadata = f'tbd-version={version[1] if version else "unknown"}; target lists={reported or "unrecognized"}'

    def reject(reason):
        raise ValueError(f'{reason}; {metadata}')

    if not lists or frozenset(lists[0]) not in IOS_STUB_TARGETS:
        reject('OpenGLES.tbd does not list a known iOS target set')
    targets = frozenset(lists[0])
    if any(frozenset(items) != targets or len(items) != len(targets) for items in lists):
        reject('OpenGLES.tbd names an iOS target set the rewrite does not cover')
    if IOS_INSTALL_NAME not in text:
        reject(f'OpenGLES.tbd does not have the install name {IOS_INSTALL_NAME}')
    stub = pattern.sub(lambda match: match[1] + CATALYST_STUB_TARGETS, text)
    stub = stub.replace(IOS_INSTALL_NAME, CATALYST_INSTALL_NAME)
    if '-ios' in stub:
        reject('OpenGLES.tbd names an iOS target the rewrite does not cover')
    return stub


def opengles_framework(folder):
    """An OpenGLES.framework for the Catalyst build: the iPhoneOS SDK's headers, which the
    MacOSX SDK does not ship, and a link stub for the Mac's macCatalyst OpenGLES.framework
    (/System/iOSSupport), made from the iPhoneOS SDK's stub. Read from the installed Xcode at
    build time, never committed."""
    sdk=Path(subprocess.run(['xcrun','--sdk','iphoneos','--show-sdk-path'],capture_output=True,text=True,check=True).stdout.strip())
    source=sdk/'System/Library/Frameworks/OpenGLES.framework'
    framework=folder/'OpenGLES.framework'
    shutil.rmtree(framework,ignore_errors=True)
    shutil.copytree(source/'Headers',framework/'Headers')
    try:
        stub=maccatalyst_stub((source/'OpenGLES.tbd').read_text())
    except ValueError as error:
        sys.exit(f'{source}/OpenGLES.tbd: {error}')
    (framework/'OpenGLES.tbd').write_text(stub)
    return folder


def benchmark_upload_aps_environment(
    enabled: bool,
    app_store_package: bool,
    signing_profile: str | None,
    platform_name: str,
    container_identifier: str,
) -> str:
    if not enabled:
        return "development"
    if app_store_package and not signing_profile:
        raise ValueError(
            "--benchmark-upload App Store packages require an existing --signing-profile"
        )
    if not signing_profile:
        return "development"
    profile = release_signing.installed_profile(signing_profile)
    release_signing.validate_profile_entitlements(
        profile,
        platform_name,
        benchmark_upload=True,
        container_identifier=container_identifier,
    )
    if platform_name == 'macos':
        # the native Mac app signs CloudKit without a push entitlement (port/ios/CMakeLists.txt)
        return "development"
    return profile["Entitlements"]["aps-environment"]


def cloud_definitions(bundle_id, cloud_saves, cloud_container, benchmark_upload=False, shader_upload=False, perf_reports=False):
    """the CMake definitions for --cloud-saves and --benchmark-upload: off unless asked for, and then with a
    container, which only the project's own bundle IDs have a default for (a container belongs to one developer team)"""
    if not cloud_saves and not benchmark_upload and not shader_upload and not perf_reports:
        if cloud_container:
            raise ValueError('--cloud-container is for --cloud-saves builds')
        return ['-DHALO_CLOUD_SAVES=OFF', '-DHALO_BENCHMARK_UPLOAD=OFF', '-DHALO_SHADER_UPLOAD=OFF', '-DHALO_PERF_REPORTS=OFF']   # all named: CMake's cache keeps an earlier ON
    if not cloud_container:
        if bundle_id != 'org.steverice.visr' and not bundle_id.startswith('org.steverice.visr.'):
            raise ValueError(f'--cloud-saves with the bundle ID {bundle_id} needs --cloud-container '
                             '(an iCloud container of your own team, e.g. iCloud.com.yourname.visr)')
        cloud_container = 'iCloud.org.steverice.visr'
    if not re.fullmatch(r'iCloud\.[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', cloud_container):
        raise ValueError('--cloud-container must look like iCloud.com.yourname.visr')
    return [f'-DHALO_CLOUD_SAVES={"ON" if cloud_saves else "OFF"}', f'-DHALO_CLOUD_CONTAINER={cloud_container}',
            f'-DHALO_BENCHMARK_UPLOAD={"ON" if benchmark_upload else "OFF"}',
            f'-DHALO_SHADER_UPLOAD={"ON" if shader_upload else "OFF"}',
            f'-DHALO_PERF_REPORTS={"ON" if perf_reports else "OFF"}']


def data_pack_settings(args):
    """the key and feed URLs a build gets: the project's own unless named, none with --data-packs off"""
    explicit = [args.data_pack_public_key, args.profile_feed_url, args.recording_feed_url, args.shader_feed_url]
    named = any(value is not None for value in explicit)
    enabled = args.data_packs == 'on' or (args.data_packs is None and (named or not args.mac))
    if not enabled:
        if named:
            raise ValueError('--data-packs off leaves out the key and feed URLs; drop them, or use --data-packs on')
        return '', '', '', ''
    channel = getattr(args, 'data_pack_channel', None) or ('tester' if args.tester else 'stable')
    return (DATA_PACK_PUBLIC_KEY if args.data_pack_public_key is None else args.data_pack_public_key,
            DATA_PACK_FEED.format(channel=channel, kind='profiles').removesuffix('.json') if args.profile_feed_url is None else args.profile_feed_url,
            DATA_PACK_FEED.format(channel=channel, kind='recordings') if args.recording_feed_url is None
            else args.recording_feed_url,
            DATA_PACK_FEED.format(channel=channel, kind='shaders') if args.shader_feed_url is None else args.shader_feed_url)


def data_pack_definitions(public_key, profile_feed_url, recording_feed_url, shader_feed_url=""):
    """the CMake definitions for signed data updates (docs/data-packs.md), all three always named so CMake's cache
    cannot keep an earlier value; empty values leave downloads off"""
    if public_key and not re.fullmatch(r'[0-9A-Fa-f]{64}', public_key):
        raise ValueError('--data-pack-public-key must be the 32-byte Ed25519 public key as 64 hex digits')
    for name, url in (('--profile-feed-url', profile_feed_url), ('--recording-feed-url', recording_feed_url), ('--shader-feed-url', shader_feed_url)):
        if url and not feed_url_valid(url):
            raise ValueError(f'{name}: a feed URL must be https:// with a host, and without credentials, a query, '
                             'a fragment, spaces or characters outside a plain URL')
    return [f'-DHALO_DATA_PACK_PUBLIC_KEY={public_key.lower()}', f'-DHALO_PROFILE_FEED_URL={profile_feed_url}',
            f'-DHALO_RECORDING_FEED_URL={recording_feed_url}', f'-DHALO_SHADER_FEED_URL={shader_feed_url}']


def feed_url_valid(url):
    # also keeps out what Xcode would expand or split in a build setting ($, quotes, spaces) and CMake's list
    # separator (;)
    if not re.fullmatch(r"[A-Za-z0-9._~:/%+=@-]+", url) or '?' in url or '#' in url:
        return False
    parts = urllib.parse.urlsplit(url)
    try:
        parts.port
    except ValueError:
        return False
    return (parts.scheme == 'https' and bool(parts.hostname) and parts.username is None and
            parts.password is None and '@' not in parts.netloc)


def mac_build_folder(environment=None):
    """where the Catalyst build goes: build/mac/app in the checkout, or, on a host that runs developer-built
    apps only from one folder (HALO_MAC_BUILD, set by that host's job runner), a folder per
    checkout there"""
    environment = os.environ if environment is None else environment
    if environment.get('HALO_MAC_BUILD'):
        return Path(environment['HALO_MAC_BUILD']) / ROOT.name / 'app'
    return ROOT / 'build/mac/app'


def build_mac(args, cloud, data_packs=()):
    """The iOS project built for Mac Catalyst: configured as iOS, built with the macOS SDK"""
    build=mac_build_folder()
    renderer = args.renderer.upper() if args.renderer else 'BOTH'
    frameworks = opengles_framework(ROOT/'build/mac/frameworks') if renderer == 'BOTH' else ''
    import time
    routes = args.benchmark_routes or 'on'
    run('cmake','-S','port/ios','-B',build,'-G','Xcode','-DCMAKE_SYSTEM_NAME=iOS','-DCMAKE_OSX_SYSROOT=iphoneos',
        '-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=16.0',f'-DHALO_BUNDLE_IDENTIFIER={args.bundle_id}',
        f'-DHALO_DEVELOPMENT_TEAM={args.team or ""}','-DHALO_MAC=ON',f'-DHALO_RENDERER={renderer}',f'-DHALO_GLES_FRAMEWORKS={frameworks}',f'-DHALO_BINK={args.bink.upper()}',
        f'-DHALO_BENCHMARK_ROUTES={routes.upper()}',
        f'-DHALO_RENDER_VALIDATION={"ON" if args.render_validation == "on" else "OFF"}',
        f'-DHALO_BUILD_NUMBER={time.strftime("%Y%m%d.%H%M%S")}',
        *([f'-DHALO_MARKETING_VERSION={args.marketing_version}'] if args.marketing_version else []),
        *([f'-DHALO_FRAME_PACING={args.frame_pacing.upper()}'] if args.frame_pacing else []),*cloud,*data_packs,
        *host_pgo_flags(args.host_pgo))
    # ad hoc, except with --cloud-saves or --benchmark-upload: iCloud entitlements need a development signature and a
    # Mac profile (and the Mac itself registered with the team, which a Mac profile lists as its device)
    signing=(['-allowProvisioningUpdates','-allowProvisioningDeviceRegistration'] if args.cloud_saves or args.benchmark_upload or args.shader_upload or args.perf_reports else
             ['CODE_SIGN_STYLE=Manual','CODE_SIGN_IDENTITY=-','DEVELOPMENT_TEAM='])
    drop_stale_ffmpeg_license(build, args.bink)
    run('cmake','--build',build,'--config','Release','--target','VISR','--','-quiet',
        '-sdk','macosx','SDK_VARIANT=iosmac','SUPPORTS_MACCATALYST=YES','DERIVE_MACCATALYST_PRODUCT_BUNDLE_IDENTIFIER=NO',
        *signing)
    print(f'App: {build}/Release-maccatalyst/VISR.app')


if __name__=='__main__':main()
