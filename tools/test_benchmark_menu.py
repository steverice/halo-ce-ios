#!/usr/bin/env python3
"""Build and run the UIKit menu probe as a small Mac Catalyst app (no game data)."""
import os
import plistlib
import subprocess
from pathlib import Path

def main():
    ROOT = Path(__file__).resolve().parents[1]
    BUILD = ROOT / 'build/ios/benchmark-menu-probe'
    APP = BUILD / 'BenchmarkMenuProbe.app'
    CONTENTS = APP / 'Contents'
    MACOS = CONTENTS / 'MacOS'
    MACOS.mkdir(parents=True, exist_ok=True)
    SDK = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    subprocess.run([
        'xcrun', 'clang', '-fobjc-arc', '-target', 'arm64-apple-ios15.0-macabi', '-isysroot', SDK,
        '-iframework', f'{SDK}/System/iOSSupport/System/Library/Frameworks',
        '-Iport/ios/host', '-Iport/runtime', '-Iport/runtime/include', '-DHALO_BENCHMARK_CLOUD=1',
        'port/ios/tests/benchmark_menu_probe.m', '-framework', 'UIKit', '-framework', 'Foundation', '-framework', 'CoreText', '-framework', 'CoreGraphics', '-framework', 'QuartzCore',
        '-o', str(MACOS / 'BenchmarkMenuProbe'),
    ], cwd=ROOT, check=True)
    with (CONTENTS / 'Info.plist').open('wb') as output:
        plistlib.dump(dict(CFBundleExecutable='BenchmarkMenuProbe', CFBundleIdentifier='org.visr.tests.benchmark-menu',
                           CFBundleName='BenchmarkMenuProbe', CFBundlePackageType='APPL',
                           LSRequiresIPhoneOS=True, UIApplicationSupportsIndirectInputEvents=True,
                           UIApplicationSceneManifest=dict(UIApplicationSupportsMultipleScenes=False,
                               UISceneConfigurations={'UIWindowSceneSessionRoleApplication': [dict(
                                   UISceneConfigurationName='Default', UISceneDelegateClassName='ProbeDelegate')]})), output)
    subprocess.run(['codesign', '--force', '--sign', '-', str(APP)], check=True)
    subprocess.run([str(MACOS / 'BenchmarkMenuProbe')], cwd=ROOT, check=True, timeout=30,
                   env={**os.environ, 'DYLD_FRAMEWORK_PATH': '/System/iOSSupport/System/Library/Frameworks'})


if __name__ == '__main__':
    main()
