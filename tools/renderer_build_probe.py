#!/usr/bin/env python3
"""Probe renderer build selection against its C and CMake source."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def run(*command):
    subprocess.run([str(part) for part in command], cwd=ROOT, check=True)


def main():
    host_main = (ROOT / 'port/ios/host/host_main.m').read_text()
    if host_main.index('host_renderer_guest_value') > host_main.index('copy_string(env[i])'):
        raise SystemExit('host_main selects the guest renderer after its environment snapshot')
    sdl_platform = (ROOT / 'port/linux/src/sdl_platform.c').read_text()
    if 'host_renderer_selects_metal(renderer)' not in sdl_platform:
        raise SystemExit('SDL window selection does not use the build-aware renderer policy')
    replay_source = (ROOT / 'port/ios/host/host_render_replay.c').read_text()
    if replay_source.index('host_render_replay_rejection') > replay_source.index('window = host_sdl_create_window'):
        raise SystemExit('render replay can create an OpenGL window before rejecting an unsupported capture')

    with tempfile.TemporaryDirectory(prefix='visr-renderer-probe-') as temporary:
        output = Path(temporary)
        for mode in ('both', 'metal'):
            binary = output / f'renderer-{mode}'
            run('xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                f'-DHALO_METAL_ONLY={1 if mode == "metal" else 0}',
                'port/ios/tests/renderer_build_probe.c', '-o', binary)
            run(binary)

        dispatch = output / 'host-gpu-dispatch.o'
        run('xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-c',
            '-DHALO_METAL_ONLY=1', '-DHALO_RENDER_VALIDATION=0',
            '-Iport/ios/host', '-Iport/linux/src', '-Iport/runtime/include',
            'port/ios/host/host_gpu_dispatch.c',
            '-o', dispatch)
        symbols = subprocess.run(['/usr/bin/nm', '-u', str(dispatch)], cwd=ROOT, check=True,
                                 capture_output=True, text=True).stdout
        if 'gpu_backend_gl' in symbols:
            raise SystemExit('Metal-only dispatch still references gpu_backend_gl')

        ios_build = output / 'ios-cache'
        run('cmake', '-S', 'port/ios/tests/cmake/renderer_options', '-B', ios_build,
            '-DCMAKE_SYSTEM_NAME=iOS', '-DHALO_RENDERER=METAL')
        run('cmake', '-S', 'port/ios/tests/cmake/renderer_options', '-B', ios_build,
            '-DCMAKE_SYSTEM_NAME=iOS', '-DHALO_RENDERER=BOTH')
        ios_cache = (ios_build / 'CMakeCache.txt').read_text()
        if 'SDL_OPENGLES:BOOL=ON' not in ios_cache:
            raise SystemExit('switching iOS from METAL to BOTH did not restore SDL OpenGL ES')

        tvos_build = output / 'tvos-cache'
        run('cmake', '-S', 'port/ios/tests/cmake/renderer_options', '-B', tvos_build,
            '-DCMAKE_SYSTEM_NAME=tvOS', '-DHALO_RENDERER=BOTH', '-DSDL_OPENGLES:BOOL=OFF')
        tvos_cache = (tvos_build / 'CMakeCache.txt').read_text()
        if 'SDL_OPENGLES:BOOL=ON' in tvos_cache:
            raise SystemExit('tvOS renderer setup enabled SDL OpenGL ES instead of keeping its existing override')

        vision_build = output / 'visionos-cache'
        run('cmake', '-S', 'port/ios/tests/cmake/renderer_options', '-B', vision_build,
            '-DCMAKE_SYSTEM_NAME=visionOS', '-DHALO_RENDERER=METAL', '-DSDL_OPENGLES=ON')
        vision_cache = (vision_build / 'CMakeCache.txt').read_text()
        if 'SDL_OPENGLES:BOOL=OFF' not in vision_cache:
            raise SystemExit('visionOS renderer setup did not keep SDL OpenGL ES disabled')


if __name__ == '__main__':
    main()
