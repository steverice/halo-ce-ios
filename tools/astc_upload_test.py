#!/usr/bin/env python3
"""Exercise production ASTC upload/cache code without linking the game.

Run through the job router. --tree can point at an uncommitted integration
worktree; this script and its fixture need not be copied into that tree.
"""
import argparse
from pathlib import Path
import re
import subprocess


def function(source, name):
    match = re.search(r'^[\w *]+\b' + re.escape(name) + r'\([^;]*?\)\n\{', source, re.M)
    if not match:
        raise ValueError(f"Cannot find production function {name}")
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build', type=Path)
    args = parser.parse_args()
    tree = args.tree.resolve()
    build = args.build or tree / 'build/ios/probe/astc-upload'
    build.mkdir(parents=True, exist_ok=True)
    source = (tree / 'port/linux/src/xbox_textures.c').read_text()
    header = (tree / 'port/linux/src/xgpu.h').read_text()
    backend = (tree / 'port/ios/host/gpu_metal.m').read_text()
    fixture = (Path(__file__).resolve().parents[1] / 'port/ios/tests/astc_upload_probe.c.in').read_text()
    description = header[header.index('struct xgpu_texture_description\n'):header.index('\nvoid xgpu_texture_describe')]
    formats = source[source.index('enum texel_kind\n'):source.index('/* ---------- geometry')]
    cache = source[source.index('struct texture_entry\n'):source.index('/* ---------- debug.texture_override_directory')]
    layout = '\n'.join(function(source, name) for name in (
        'level_dimension', 'level_bytes', 'xgpu_texture_level_offset',
        'xgpu_texture_face_size', 'texture_size_supported'))
    helpers = '\n'.join(function(source, name) for name in (
        'texture_entry_format', 'texture_astc_prepare', 'texture_astc_key',
        'texture_astc_bytes', 'texture_astc_stored', 'texture_astc_upload',
        'texture_gpu_bytes', 'texture_astc_swap_ready'))
    # Preserve the complete production selection block. Only hardware queries
    # require substitution to compile this Objective-C block as portable C.
    start = backend.index('compressed_textures = astc_textures = NO;')
    end = backend.index('\n\n\t\t/*', start)
    initialization = backend[start:end]
    initialization = re.sub(r'@available\([^)]*\)', 'fixture_bc_api', initialization)
    initialization = initialization.replace('device.supportsBCTextureCompression', 'fixture_bc')
    initialization = initialization.replace('[device supportsFamily:MTLGPUFamilyApple2]', 'fixture_astc')
    for token, value in {
        'DESCRIPTION': description, 'FORMATS': formats, 'LAYOUT': layout,
        'CACHE': cache, 'HELPERS': helpers,
        'RESULT': function(source, 'texture_entry_result_uncached') + function(source, 'texture_entry_result'),
        'GET': function(source, 'xgpu_texture_get'), 'INITIALIZATION': initialization,
    }.items():
        fixture = fixture.replace('@' + token + '@', value)
    generated = build / 'astc_upload_probe.c'
    generated.write_text(fixture)
    executable = build / 'astc-upload-probe'
    subprocess.run(['xcrun', 'clang', '-O1', '-g', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', '--include-directory=' + str(tree / 'port/linux/src'),
                    str(generated), '--output', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
