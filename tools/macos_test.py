#!/usr/bin/env python3
"""Run asset-free native AppKit probes without launching the application."""
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/macos/probe'


def run(*arguments):
    print('+ ' + shlex.join(str(argument) for argument in arguments), flush=True)
    subprocess.run([str(argument) for argument in arguments], cwd=ROOT, check=True)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    sdl_flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
    common = ['xcrun', '--sdk', 'macosx', 'clang', '-O2', '-mmacosx-version-min=14.0',
              '-DHALO_IOS=1', '-Iport/macos', '-Iport/ios/host', '-Iport/runtime',
              '-Iport/runtime/include', '-Wl,-dead_strip']
    objc = ['-fobjc-arc', '-fblocks', '-framework', 'AppKit']
    # These include the real implementation and replace only its SDL/file boundaries.
    run(*common, '-DHALO_NATIVE_MAC=1', *objc, 'tools/macos_import_test.m',
        '-framework', 'UniformTypeIdentifiers', '-o', BUILD / 'import-probe')
    run(BUILD / 'import-probe')
    run(*common, '-DHALO_NATIVE_MAC=1', *objc, 'tools/macos_services_test.m',
        'port/ios/host/host_frame_counter_stats.c', '-framework', 'QuartzCore',
        '-framework', 'CoreGraphics', *sdl_flags, '-o', BUILD / 'services-probe')
    run(BUILD / 'services-probe')
    run('python3', 'tools/test_menu_display_rate.py')
    # Verify both the native overrides and the shared host's original mobile flags.
    for native, name in ((1, 'window-native-probe'), (0, 'window-mobile-probe')):
        run(*common, f'-DHALO_NATIVE_MAC={native}', 'tools/macos_window_test.c',
            'port/ios/host/host_config.c', *sdl_flags, '-o', BUILD / name)
        run(BUILD / name)


if __name__ == '__main__':
    main()
