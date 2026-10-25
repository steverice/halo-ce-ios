#!/usr/bin/env python3
"""Exercise the production ASTC encoder against Arm's decoder."""
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/ios/probe/astc-encode'


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    cc = ['xcrun', 'clang'] if os.uname().sysname == 'Darwin' else ['cc']
    cxx = ['xcrun', 'clang++'] if os.uname().sysname == 'Darwin' else ['c++']
    flags = ['-O2', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']
    subprocess.run([*cc, *flags, '--include-directory=port/linux/src', '-c',
                    'port/linux/src/astc_rewrite.c', '--output', str(BUILD / 'rewrite.o')], cwd=ROOT, check=True)
    source = (ROOT / 'port/linux/src/xbox_textures.c').read_text()
    functions = []
    for name in ('expand5', 'expand6', 'expand4', 'argb', 'color565', 'mix', 'dxt_color_block', 'dxt_decode_level'):
        match = re.search(r'^static [^;{]*\b' + name + r'\(', source, re.MULTILINE)
        start = match.start()
        end = source.index('{', start) + 1
        depth = 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        functions.append(source[start:end])
    reference = '#include <stdint.h>\n#define TRUE 1\n#define FALSE 0\ntypedef int BOOL;\n'
    reference += 'enum { _texel_dxt1, _texel_dxt3, _texel_dxt5 };\n' + '\n'.join(functions)
    reference += '\nvoid dxt_reference(int f,const uint8_t *b,unsigned long *p) { dxt_decode_level(f,b,4,4,1,p); }\n'
    (BUILD / 'reference.c').write_text(reference)
    subprocess.run([*cc, *flags, '-c', str(BUILD / 'reference.c'), '--output', str(BUILD / 'reference.o')], cwd=ROOT, check=True)
    vendor = ROOT / 'port/third_party/astcenc'
    subprocess.run([*cxx, *flags, '-std=c++14', '-DASTCENC_BLOCK_MAX_TEXELS=16',
                    '--include-directory=port/linux/src', f'--include-directory={vendor}',
                    'port/ios/tests/astc_encode_probe.cpp', 'port/linux/src/astc_encode.cpp',
                    *map(str, sorted(vendor.glob('*.cpp'))), str(BUILD / 'rewrite.o'), str(BUILD / 'reference.o'),
                    '--output', str(BUILD / 'probe')], cwd=ROOT, check=True)
    subprocess.run([str(BUILD / 'probe')], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
