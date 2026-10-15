#!/usr/bin/env python3
"""Build the iPhone/iPad (or, with --tvos, Apple TV; with --visionos, Apple Vision Pro) app from source on an Apple Silicon Mac."""
import argparse
import os
import platform
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request

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


def run(*args):
    print('+', ' '.join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--simulator', action='store_true', help='build for an ARM64 simulator')
    mode.add_argument('--unsigned', action='store_true', help='build a device app for signing later')
    parser.add_argument('--team', help='Apple development team ID for device signing')
    parser.add_argument('--bundle-id', help='bundle identifier covered by your signing profile (default org.steverice.visr for iOS, tvOS and visionOS, so one App Store Connect record covers them all; org.steverice.visr.mac for --mac)')
    target = parser.add_mutually_exclusive_group()
    target.add_argument('--tvos', action='store_true', help='build for Apple TV instead of iPhone/iPad')
    target.add_argument('--visionos', action='store_true', help='build for Apple Vision Pro (Metal only) instead of iPhone/iPad')
    target.add_argument('--mac', action='store_true', help='build the iOS host for Mac Catalyst, ad hoc signed (the native Mac runner)')
    parser.add_argument('--render-height', type=int,
                        help='tvOS and visionOS: internal render height in pixels, 0 for native '
                             '(default 1080 on tvOS; native on visionOS, which follows the window\'s size)')
    parser.add_argument('--extended-virtual-addressing', action='store_true',
                        help='visionOS and tvOS: sign with the extended virtual addressing entitlement (paid developer '
                             'teams), in case the device refuses the 4 GB guest arena')
    parser.add_argument('--frame-pacing', choices=('on','off'),
                        help="compile in the Metal renderer's presentation pacing (HALO_FRAME_PACING; "
                             'default on, off for --visionos, whose frames the Compositor paces)')
    parser.add_argument('--register-device', metavar='UDID',
                        help='signed device builds: build for this one device and let Xcode register it with the team '
                             '(-allowProvisioningDeviceRegistration). Off by default: a build never registers a device unasked')
    parser.add_argument('--cloud-saves', action='store_true',
                        help='sync saves through the player\'s private iCloud database (docs/cloud-saves.md). The App ID '
                             'needs the iCloud capability with CloudKit and the container; a signed build with this '
                             'option lets Xcode add them to your developer account')
    parser.add_argument('--cloud-container',
                        help='--cloud-saves: the iCloud container (default iCloud.org.steverice.visr for the '
                             'org.steverice.visr bundle IDs; required with any other bundle ID)')
    parser.add_argument('--benchmark-upload', action='store_true',
                        help='let the device benchmark send its report, when its tester agrees, to the public database '
                             'of the iCloud container (--cloud-container, or the project\'s own). Signs with the iCloud '
                             'entitlement as --cloud-saves does, without turning cloud saves on')
    parser.add_argument('--ipa', type=Path, help='also package the device app at this path')
    parser.add_argument('--tester', action='store_true',
                        help='a tester build: the Run benchmark row at launch, and the privacy manifest that declares '
                             'the benchmark report (port/ios/privacy)')
    parser.add_argument('--benchmark-routes', choices=('on','off'),
                        help="bundle the device benchmark's recorded routes and run them as scenes (default on; off "
                             'for --tester, whose suite has only the scenes that play by themselves until recorded '
                             'routes are shown to hold on other devices)')
    parser.add_argument('--archive-only', action='store_true',
                        help='with --app-store-package: stop after the archive. The export that follows is what '
                             'makes the distribution certificate and App Store profile')
    parser.add_argument('--app-store-package', type=Path, metavar='FOLDER',
                        help='signed device builds: also archive the app and export an App Store Connect package '
                             '(for TestFlight) into this folder. Xcode makes the team\'s distribution certificate and '
                             'App Store profile if they are missing, which changes the developer account')
    parser.add_argument('--upload', action='store_true',
                        help='with --app-store-package: upload the package to App Store Connect with the account '
                             'Xcode is signed in to. The app record must exist there first')
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
    if not args.simulator and not args.unsigned and not args.team and not args.mac:
        parser.error('use --team YOUR_TEAM_ID to sign, or --unsigned to sign later')
    if args.team and (args.unsigned or args.simulator):
        parser.error('--team is only used for signed device builds')
    if args.register_device and not args.team:
        parser.error('--register-device is for signed device builds (--team)')
    if args.ipa and args.simulator:
        parser.error('--ipa requires a device build')
    if args.app_store_package and not args.team:
        parser.error('--app-store-package is for signed device builds (--team)')
    if args.upload and not args.app_store_package:
        parser.error('--upload needs --app-store-package')
    if args.archive_only and (not args.app_store_package or args.upload):
        parser.error('--archive-only goes with --app-store-package and without --upload')
    if args.tester and args.mac:
        parser.error('--tester is for device and simulator builds')
    if args.benchmark_upload and (args.simulator or args.unsigned):
        parser.error('--benchmark-upload needs a signed build: the iCloud entitlement comes from the signature')
    if args.render_height is None:
        # 0: the device profile's (profiles/); a number here overrides every profile
        args.render_height = 0
    if args.extended_virtual_addressing and not (args.visionos or args.tvos):
        parser.error('--extended-virtual-addressing is for --visionos and --tvos builds')
    platform_name = 'tvos' if args.tvos else 'visionos' if args.visionos else 'mac' if args.mac else 'ios'
    args.bundle_id = args.bundle_id or ('org.steverice.visr.mac' if platform_name == 'mac' else 'org.steverice.visr')
    if args.mac and (args.simulator or args.unsigned or (args.team and not (args.cloud_saves or args.benchmark_upload))):
        parser.error('--mac builds are ad hoc signed; drop --simulator, --unsigned and --team')
    try:
        cloud = cloud_definitions(args.bundle_id, args.cloud_saves, args.cloud_container, args.benchmark_upload)
    except ValueError as error:
        parser.error(str(error))
    if args.mac and (args.cloud_saves or args.benchmark_upload) and not args.team:
        parser.error('--mac with --cloud-saves or --benchmark-upload needs --team: macOS does not run an ad hoc signed '
                     'app with iCloud entitlements')
    if args.cloud_saves and args.team:
        print(f'iCloud saves: this signed build lets Xcode enable the iCloud capability and the container '
              f'{cloud[1].split("=", 1)[1]} on the App ID {args.bundle_id} and make a new profile', flush=True)
    if not re.fullmatch(r'[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', args.bundle_id):
        parser.error('--bundle-id must be a reverse-DNS identifier (e.g. com.example.halo)')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    include=ROOT/'build/ios/gl_include'
    # visionOS has no OpenGL ES; its build leaves the GL backend out
    for name,(registry,revision,source) in ({} if args.visionos else HEADERS).items():
        target=include/name;target.parent.mkdir(parents=True,exist_ok=True)
        url=f'https://raw.githubusercontent.com/KhronosGroup/{registry}/{revision}/{source}'
        data=urllib.request.urlopen(url,timeout=30).read()
        if not target.exists() or target.read_bytes()!=data:target.write_bytes(data)
    validation = args.render_validation == 'on'
    run(sys.executable,'configure.py','--ios',f'--pgo={args.pgo}',f'--lto={args.lto}','--ios-llvm',args.llvm,'--ios-lld',args.lld,
        *(['--render-validation'] if validation else []))
    run('ninja','ios_guest','-j',args.jobs)
    run(sys.executable,'tools/ios_bridges.py',*(['--render-validation'] if validation else []))
    # what a render capture is checked against when it is replayed (port/ios/host/gpu_capture.h)
    if validation: run(sys.executable,'tools/gpu_interface_hash.py')
    run(sys.executable,'tools/ios_embed_guest.py','build/ios/halo_guest.elf','build/ios/embedded')
    notices = ROOT/'build/ios/licenses'
    notices.mkdir(parents=True, exist_ok=True)
    for source, name in (
        ('LICENSE.md', 'Project-CC0.txt'),
        ('port/ios/THIRD_PARTY.md', 'NOTICE.txt'),
        ('build/third_party/SDL3/LICENSE.txt', 'SDL.txt'),
        ('build/third_party/musl-1.2.5/COPYRIGHT', 'musl.txt'),
        ('port/third_party/kcp/LICENSE', 'kcp.txt'),
        ('port/third_party/extract-xiso/LICENSE.TXT', 'extract-xiso.txt'),
        ('port/third_party/tomlc17/LICENSE', 'tomlc17.txt'),
        ('port/third_party/stb/LICENSE', 'stb_truetype.txt'),
        ('port/third_party/expat/COPYING', 'expat.txt'),
        ('port/third_party/monocypher/LICENCE.md', 'Monocypher.txt'),
        ('port/third_party/mbedtls/LICENSE', 'MbedTLS-Apache-2.0.txt'),
        ('port/third_party/zlib/LICENSE', 'zlib.txt'),
        ('port/assets/fonts/Overpass-OFL.txt', 'Overpass-OFL.txt'),
        ('port/assets/fonts/OpenCE-OFL.txt', 'OpenCE-OFL.txt'),
        ('port/assets/fonts/Newtown-LICENSE.txt', 'Newtown.txt'),
        ('port/third_party/smaa/LICENSE', 'SMAA.txt'),
    ):
        shutil.copyfile(ROOT/source, notices/name)
    if args.mac:
        build_mac(args, cloud)
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
             f'-DHALO_RENDER_HEIGHT={args.render_height}',
             f'-DHALO_EXTENDED_VIRTUAL_ADDRESSING={"ON" if args.extended_virtual_addressing else "OFF"}', *cloud,
             f'-DHALO_RENDER_VALIDATION={"ON" if validation else "OFF"}',
             *host_pgo_flags(args.host_pgo)]
    if args.frame_pacing:
        command.append(f'-DHALO_FRAME_PACING={args.frame_pacing.upper()}')
    command.append(f'-DHALO_TESTER_BUILD={"ON" if args.tester else "OFF"}')
    routes = args.benchmark_routes or ('off' if args.tester else 'on')
    command.append(f'-DHALO_BENCHMARK_ROUTES={routes.upper()}')
    # a new CFBundleVersion each build, so an install over the app replaces it (on an Apple TV that keeps
    # the imported maps: an uninstall would take them, and the upload with them)
    import time
    command.append(f'-DHALO_BUILD_NUMBER={time.strftime("%Y%m%d.%H%M%S")}')
    run(*command)
    # an App Store archive leaves the product as a link into its own intermediates, which the next plain build
    # cannot write through ("unable to create directory"): a link there is removed first
    product = build/f'Release-{sdk}/VISR.app'
    if product.is_symlink():
        product.unlink()
    command=['cmake','--build',build,'--config','Release','--target','VISR','--','-quiet']
    if args.simulator or args.unsigned:command.append('CODE_SIGNING_ALLOWED=NO')
    else:
        command.append('-allowProvisioningUpdates')
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
        commands = app_store_commands(build, system, args.team, args.app_store_package.resolve(), args.upload)
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
        for command in [] if args.archive_only else commands[1:]:
            run(*command)
        print(f'App Store Connect package: {args.app_store_package}' +
              (' (archive only: not exported)' if args.archive_only else ' (uploaded)' if args.upload else ''))


def app_store_options(team, upload):
    """the export options for an App Store Connect package: signed for distribution by Xcode's automatic signing,
    and either written to disk or uploaded"""
    return {'method': 'app-store-connect', 'destination': 'upload' if upload else 'export', 'teamID': team,
            'signingStyle': 'automatic', 'manageAppVersionAndBuildNumber': False, 'uploadSymbols': True}


def app_store_commands(build, system, team, out, upload):
    """the commands that archive the configured project and export (or upload) its App Store Connect package into
    out; writes out/ExportOptions.plist for the second"""
    import plistlib
    out.mkdir(parents=True, exist_ok=True)
    (out/'ExportOptions.plist').write_bytes(plistlib.dumps(app_store_options(team, upload)))
    archive = out/'VISR.xcarchive'
    return [['xcodebuild', '-project', build/'VISR.xcodeproj', '-scheme', 'VISR', '-configuration', 'Release',
             '-destination', f'generic/platform={system}', '-archivePath', archive, '-quiet', '-allowProvisioningUpdates',
             'archive'],
            ['xcodebuild', '-exportArchive', '-archivePath', archive, '-exportOptionsPlist', out/'ExportOptions.plist',
             '-exportPath', out, '-allowProvisioningUpdates']]

# the iPhoneOS SDK's OpenGLES.tbd (Xcode 27), and what the Catalyst link stub says in its place
IOS_STUB_TARGETS = '[ arm64e-ios, arm64e.x1-ios ]'
CATALYST_STUB_TARGETS = '[ arm64-maccatalyst, arm64e-maccatalyst ]'
IOS_INSTALL_NAME = "'/System/Library/Frameworks/OpenGLES.framework/OpenGLES'"
CATALYST_INSTALL_NAME = "'/System/iOSSupport/System/Library/Frameworks/OpenGLES.framework/OpenGLES'"


def maccatalyst_stub(text):
    """the iPhoneOS SDK's OpenGLES.tbd rewritten to link against the Mac's macCatalyst
    OpenGLES.framework. A stub in any other form (a later Xcode's) is refused, not half rewritten."""
    if IOS_STUB_TARGETS not in text:
        raise ValueError(f'OpenGLES.tbd does not list {IOS_STUB_TARGETS}')
    if IOS_INSTALL_NAME not in text:
        raise ValueError(f'OpenGLES.tbd does not have the install name {IOS_INSTALL_NAME}')
    stub = text.replace(IOS_STUB_TARGETS, CATALYST_STUB_TARGETS).replace(IOS_INSTALL_NAME, CATALYST_INSTALL_NAME)
    if '-ios' in stub:
        raise ValueError('OpenGLES.tbd names an iOS target the rewrite does not cover')
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


def cloud_definitions(bundle_id, cloud_saves, cloud_container, benchmark_upload=False):
    """the CMake definitions for --cloud-saves and --benchmark-upload: off unless asked for, and then with a
    container, which only the project's own bundle IDs have a default for (a container belongs to one developer team)"""
    if not cloud_saves and not benchmark_upload:
        if cloud_container:
            raise ValueError('--cloud-container is for --cloud-saves builds')
        return ['-DHALO_CLOUD_SAVES=OFF', '-DHALO_BENCHMARK_UPLOAD=OFF']   # both named: CMake's cache keeps an earlier ON
    if not cloud_container:
        if bundle_id != 'org.steverice.visr' and not bundle_id.startswith('org.steverice.visr.'):
            raise ValueError(f'--cloud-saves with the bundle ID {bundle_id} needs --cloud-container '
                             '(an iCloud container of your own team, e.g. iCloud.com.yourname.visr)')
        cloud_container = 'iCloud.org.steverice.visr'
    if not re.fullmatch(r'iCloud\.[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', cloud_container):
        raise ValueError('--cloud-container must look like iCloud.com.yourname.visr')
    return [f'-DHALO_CLOUD_SAVES={"ON" if cloud_saves else "OFF"}', f'-DHALO_CLOUD_CONTAINER={cloud_container}',
            f'-DHALO_BENCHMARK_UPLOAD={"ON" if benchmark_upload else "OFF"}']


def mac_build_folder(environment=None):
    """where the Catalyst build goes: build/mac/app in the checkout, or, on a host that runs developer-built
    apps only from one folder (HALO_MAC_BUILD, set by that host's job runner), a folder per
    checkout there"""
    environment = os.environ if environment is None else environment
    if environment.get('HALO_MAC_BUILD'):
        return Path(environment['HALO_MAC_BUILD']) / ROOT.name / 'app'
    return ROOT / 'build/mac/app'


def build_mac(args, cloud):
    """The iOS project built for Mac Catalyst: configured as iOS, built with the macOS SDK"""
    build=mac_build_folder()
    frameworks=opengles_framework(ROOT/'build/mac/frameworks')
    import time
    run('cmake','-S','port/ios','-B',build,'-G','Xcode','-DCMAKE_SYSTEM_NAME=iOS','-DCMAKE_OSX_SYSROOT=iphoneos',
        '-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=16.0',f'-DHALO_BUNDLE_IDENTIFIER={args.bundle_id}',
        f'-DHALO_DEVELOPMENT_TEAM={args.team or ""}','-DHALO_MAC=ON',f'-DHALO_GLES_FRAMEWORKS={frameworks}',
        f'-DHALO_RENDER_VALIDATION={"ON" if args.render_validation == "on" else "OFF"}',
        f'-DHALO_BUILD_NUMBER={time.strftime("%Y%m%d.%H%M%S")}',
        *([f'-DHALO_FRAME_PACING={args.frame_pacing.upper()}'] if args.frame_pacing else []),*cloud,
        *host_pgo_flags(args.host_pgo))
    # ad hoc, except with --cloud-saves or --benchmark-upload: iCloud entitlements need a development signature and a
    # Mac profile (and the Mac itself registered with the team, which a Mac profile lists as its device)
    signing=(['-allowProvisioningUpdates','-allowProvisioningDeviceRegistration'] if args.cloud_saves or args.benchmark_upload else
             ['CODE_SIGN_STYLE=Manual','CODE_SIGN_IDENTITY=-','DEVELOPMENT_TEAM='])
    run('cmake','--build',build,'--config','Release','--target','VISR','--','-quiet',
        '-sdk','macosx','SDK_VARIANT=iosmac','SUPPORTS_MACCATALYST=YES','DERIVE_MACCATALYST_PRODUCT_BUNDLE_IDENTIFIER=NO',
        *signing)
    print(f'App: {build}/Release-maccatalyst/VISR.app')


if __name__=='__main__':main()
