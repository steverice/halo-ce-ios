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

def run(*args):
    print('+', ' '.join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--simulator', action='store_true', help='build for an ARM64 simulator')
    mode.add_argument('--unsigned', action='store_true', help='build a device app for signing later')
    parser.add_argument('--team', help='Apple development team ID for device signing')
    parser.add_argument('--bundle-id', help='bundle identifier covered by your signing profile (default org.steverice.visr, or org.steverice.visr.tvos / .visionos / .mac for --tvos / --visionos / --mac)')
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
    parser.add_argument('--ipa', type=Path, help='also package the device app at this path')
    parser.add_argument('--tester', action='store_true',
                        help='a tester build: the Run benchmark row at launch, and the privacy manifest that declares '
                             'the benchmark report (port/ios/privacy)')
    parser.add_argument('--app-store-package', type=Path, metavar='FOLDER',
                        help='signed device builds: also archive the app and export an App Store Connect package '
                             '(for TestFlight) into this folder. Xcode makes the team\'s distribution certificate and '
                             'App Store profile if they are missing, which changes the developer account')
    parser.add_argument('--upload', action='store_true',
                        help='with --app-store-package: upload the package to App Store Connect with the account '
                             'Xcode is signed in to. The app record must exist there first')
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
    if args.tester and args.mac:
        parser.error('--tester is for device and simulator builds')
    if args.render_height is None:
        args.render_height = 0 if args.visionos else 1080
    if args.extended_virtual_addressing and not (args.visionos or args.tvos):
        parser.error('--extended-virtual-addressing is for --visionos and --tvos builds')
    platform_name = 'tvos' if args.tvos else 'visionos' if args.visionos else 'mac' if args.mac else 'ios'
    args.bundle_id = args.bundle_id or ('org.steverice.visr' if platform_name == 'ios' else f'org.steverice.visr.{platform_name}')
    if args.mac and (args.simulator or args.unsigned or args.team):
        parser.error('--mac builds are ad hoc signed; drop --simulator, --unsigned and --team')
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
    run(sys.executable,'configure.py','--ios','--pgo=off','--lto=off','--ios-llvm',args.llvm,'--ios-lld',args.lld)
    run('ninja','ios_guest','-j',args.jobs)
    run(sys.executable,'tools/ios_bridges.py')
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
    ):
        shutil.copyfile(ROOT/source, notices/name)
    if args.mac:
        build_mac(args)
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
             f'-DHALO_EXTENDED_VIRTUAL_ADDRESSING={"ON" if args.extended_virtual_addressing else "OFF"}']
    if args.frame_pacing:
        command.append(f'-DHALO_FRAME_PACING={args.frame_pacing.upper()}')
    command.append(f'-DHALO_TESTER_BUILD={"ON" if args.tester else "OFF"}')
    # a new CFBundleVersion each build, so an install over the app replaces it (on an Apple TV that keeps
    # the imported maps: an uninstall would take them, and the upload with them)
    import time
    command.append(f'-DHALO_BUILD_NUMBER={time.strftime("%Y%m%d.%H%M%S")}')
    run(*command)
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
        for command in app_store_commands(build, system, args.team, args.app_store_package.resolve(), args.upload):
            run(*command)
        print(f'App Store Connect package: {args.app_store_package}' + (' (uploaded)' if args.upload else ''))


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


def mac_build_folder(environment=None):
    """where the Catalyst build goes: build/mac/app in the checkout, or, on a host that runs developer-built
    apps only from one folder (HALO_MAC_BUILD, set by that host's job runner), a folder per
    checkout there"""
    environment = os.environ if environment is None else environment
    if environment.get('HALO_MAC_BUILD'):
        return Path(environment['HALO_MAC_BUILD']) / ROOT.name / 'app'
    return ROOT / 'build/mac/app'


def build_mac(args):
    """The iOS project built for Mac Catalyst: configured as iOS, built with the macOS SDK"""
    build=mac_build_folder()
    frameworks=opengles_framework(ROOT/'build/mac/frameworks')
    import time
    run('cmake','-S','port/ios','-B',build,'-G','Xcode','-DCMAKE_SYSTEM_NAME=iOS','-DCMAKE_OSX_SYSROOT=iphoneos',
        '-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=16.0',f'-DHALO_BUNDLE_IDENTIFIER={args.bundle_id}',
        '-DHALO_DEVELOPMENT_TEAM=','-DHALO_MAC=ON',f'-DHALO_GLES_FRAMEWORKS={frameworks}',
        f'-DHALO_BUILD_NUMBER={time.strftime("%Y%m%d.%H%M%S")}',
        *([f'-DHALO_FRAME_PACING={args.frame_pacing.upper()}'] if args.frame_pacing else []))
    run('cmake','--build',build,'--config','Release','--target','VISR','--','-quiet',
        '-sdk','macosx','SDK_VARIANT=iosmac','SUPPORTS_MACCATALYST=YES','DERIVE_MACCATALYST_PRODUCT_BUNDLE_IDENTIFIER=NO',
        'CODE_SIGN_STYLE=Manual','CODE_SIGN_IDENTITY=-','DEVELOPMENT_TEAM=')
    print(f'App: {build}/Release-maccatalyst/VISR.app')


if __name__=='__main__':main()
