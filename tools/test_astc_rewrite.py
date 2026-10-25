#!/usr/bin/env python3
"""Compare the direct rewrite with Arm's independent decoder and the game's DXT decoder."""
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/ios/probe/astc-rewrite'


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    # Compile the production decoder functions verbatim, so its integer rounding
    # remains the reference even if xbox_textures.c changes.
    source = (ROOT / 'port/linux/src/xbox_textures.c').read_text()
    names = ('expand5', 'expand6', 'expand4', 'argb', 'color565', 'mix', 'dxt_color_block', 'dxt_decode_level')
    functions = []
    for name in names:
        declaration = re.search(r'^static [^;{]*\b' + re.escape(name) + r'\(', source, re.MULTILINE)
        if declaration is None:
            raise RuntimeError(f'production DXT reference function missing: {name}')
        start = declaration.start()
        opening = source.index('{', start)
        depth = 1
        end = opening + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        functions.append(source[start:end])
    reference = '#include <stdint.h>\n#define TRUE 1\n#define FALSE 0\ntypedef int BOOL;\n'
    reference += 'enum { _texel_dxt1, _texel_dxt3, _texel_dxt5 };\n' + '\n'.join(functions)
    reference += '\nvoid dxt_reference(int format, const uint8_t *block, unsigned long *pixels) { dxt_decode_level((unsigned char)format, block, 4, 4, 1, pixels); }\n'
    (BUILD / 'dxt_reference.c').write_text(reference)
    cc = ['xcrun', 'clang'] if os.uname().sysname == 'Darwin' else ['cc']
    cxx = ['xcrun', 'clang++'] if os.uname().sysname == 'Darwin' else ['c++']
    flags = ['-O2', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
    for path, output in [(BUILD / 'dxt_reference.c', BUILD / 'dxt_reference.o'),
                         (ROOT / 'port/linux/src/astc_rewrite.c', BUILD / 'astc_rewrite.o')]:
        subprocess.run([*cc, *flags, '-Wall', '-Wextra', '-Werror', '--include-directory=port/linux/src',
                        '-c', str(path), '--output', str(output)], cwd=ROOT, check=True)
    vendor = ROOT / 'port/third_party/astcenc-reference'
    subprocess.run([*cxx, *flags, '-std=c++14', '-DASTCENC_DECOMPRESS_ONLY', '-DASTCENC_BLOCK_MAX_TEXELS=16',
                    '--include-directory=port/linux/src', f'--include-directory={vendor}',
                    'port/ios/tests/astc_rewrite_probe.cpp', *map(str, sorted(vendor.glob('*.cpp'))),
                    str(BUILD / 'astc_rewrite.o'), str(BUILD / 'dxt_reference.o'), '--output', str(BUILD / 'probe')],
                   cwd=ROOT, check=True)
    subprocess.run([str(BUILD / 'probe')], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
