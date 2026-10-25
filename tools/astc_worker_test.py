#!/usr/bin/env python3
"""Compile the production map worker and test synthetic files or benchmark maps."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib


def put16(data, offset, value):
    struct.pack_into('<H', data, offset, value)


def put32(data, offset, value):
    struct.pack_into('<I', data, offset, value)


def bitmap(data, offset, pixels, external=False):
    for at, value in ((4, 4), (6, 4), (12, 15), (14, 256 if external else 0)):
        put16(data, offset + at, value)
    put32(data, offset + 24, pixels)
    put32(data, offset + 28, 16)


def pixels(data, offset, alternate=False):
    # Two alpha values with constant white color pass the production quality gate.
    data[offset:offset + 16] = bytes.fromhex(
        '0f0f0f0f0f0f0f0f' if not alternate else 'f0f0f0f0f0f0f0f0') + bytes.fromhex('ffff000000000000')


def fixtures(root):
    for name, version, external, count in (
        ('xbox.map', 5, False, 1), ('pc.map', 609, False, 1),
        ('external.map', 609, True, 1), ('resume.map', 609, False, 2),
    ):
        data = bytearray(4096)
        data[:4] = b'daeh'
        base = 0x803a6000 if version == 5 else 0x40440000
        for at, value in ((4, version), (8, len(data)), (16, 2048), (20, 1500),
                          (2048, base + 40), (2060, count)):
            put32(data, at, value)
        for i in range(count):
            instance, group, element, pixel = 2088 + i * 32, 2304 + i * 256, 2416 + i * 256, 3500 + i * 16
            data[instance:instance + 4] = b'bitm'
            put32(data, instance + 20, 0 if external else base + group - 2048)
            put32(data, instance + 24, int(external))
            put32(data, group + 96, 1)
            put32(data, group + 100, base + element - 2048)
            bitmap(data, element, pixel)
            pixels(data, pixel, bool(i))
        (root / name).write_bytes(data[:2048] + zlib.compress(data[2048:]) if version == 5 else data)
    compressed = (root / 'xbox.map').read_bytes()
    cube = bytearray(compressed[:2048] + zlib.decompress(compressed[2048:]))
    # Metadata includes the 2x2 tail; guest hardware keeps only 8x8 and 4x4.
    for at, value in ((2420, 8), (2422, 8), (2426, 2), (2428, 14), (2430, 1), (2436, 2)):
        put16(cube, at, value)
    put32(cube, 2440, 3000)
    put32(cube, 2444, 680)
    for face in range(6):
        cube[3000 + face * 128:3040 + face * 128] = bytes.fromhex('ffff000000000000') * 5
    (root / 'cube.map').write_bytes(cube[:2048] + zlib.compress(cube[2048:]))
    put16(cube, 2428, 15)
    put32(cube, 2444, 720)
    for face in range(6):
        for block in range(5):
            pixels(cube, 3000 + face * 128 + block * 16, bool(face % 2))
    (root / 'cube-alpha.map').write_bytes(cube[:2048] + zlib.compress(cube[2048:]))
    put32(cube, 4, 609)
    put32(cube, 2048, 0x40440000 + 40)
    put32(cube, 2108, 0x40440000 + 256)
    put32(cube, 2404, 0x40440000 + 368)
    put32(cube, 2444, 576)
    for level, level_bytes in enumerate((64, 16, 16)):
        preceding = sum((64, 16, 16)[:level]) * 6
        for face in range(6):
            for block in range(level_bytes // 16):
                pixels(cube, 3000 + preceding + face * level_bytes + block * 16, bool(face % 2))
    (root / 'pc-cube.map').write_bytes(cube)
    resource = bytearray(1024)
    for at, value in ((0, 1), (8, 16), (12, 1), (20, 160), (24, 128), (224, 1), (228, 112)):
        put32(resource, at, value)
    bitmap(resource, 240, 512, True)
    pixels(resource, 512)
    (root / 'custom_maps').mkdir()
    (root / 'custom_maps/bitmaps.map').write_bytes(resource)
    original = bytearray((root / 'pc.map').read_bytes())
    empty = original.copy()
    put32(empty, 2060, 2)
    put32(empty, 2108, 0)
    empty[2120:2124] = b'bitm'
    put32(empty, 2140, 0x40440000 + 256)
    put32(empty, 2400, 0)
    put32(empty, 2404, 0)
    (root / 'empty.map').write_bytes(empty)
    (root / 'truncated.map').write_bytes(original[:20])
    bad = original.copy()
    put32(bad, 8, 12345)
    (root / 'bad-size.map').write_bytes(bad)
    bad = original.copy()
    put32(bad, 4, 5)
    (root / 'bad-zlib.map').write_bytes(bad[:2048] + b'bad')
    bad = original.copy()
    put32(bad, 2108, 0xffffffff)
    (root / 'bad-pointer.map').write_bytes(bad)
    bad = original.copy()
    put32(bad, 2440, 4090)
    (root / 'bad-pixels.map').write_bytes(bad)
    stable = original.copy()
    stable[32] = 73
    (root / 'stable.map').write_bytes(stable)


def inspect_stock():
    import mmap
    root = Path(os.environ['HALO_REMOTE_ROOT']) / 'data/Documents/maps'
    for name in ('a10.map', 'd40.map'):
        with (root / name).open('rb') as stream:
            raw = mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
            u32 = lambda data, at: struct.unpack_from('<I', data, at)[0]
            version, declared, tag, tag_size = [u32(raw, at) for at in (4, 8, 16, 20)]
            data = raw if len(raw) == declared else raw[:2048] + zlib.decompress(raw[2048:])
            base = 0x803a6000 if version == 5 else 0x40440000
            translation = tag - base
            instances, count = u32(data, tag) + translation, u32(data, tag + 12)
            print(name, 'version', version, 'file', len(raw), 'declared', declared, 'tag', tag, 'tag_size', tag_size, 'instances', instances, 'count', count)
            errors = 0
            for i in range(count):
                instance = instances + i * 32
                if data[instance:instance + 4] not in (b'bitm', b'mtib'):
                    continue
                pointer = u32(data, instance + 20)
                group = pointer + translation
                if group < 0 or group + 108 > len(data):
                    print('REJECT group', i, 'pointer', pointer, 'translated', group)
                    errors += 1
                    if errors >= 8:
                        break
                    continue
                n, elements = u32(data, group + 96), u32(data, group + 100) + translation
                if n > 65536 or elements < 0 or elements + n * 48 > len(data):
                    print('REJECT elements', i, 'count', n, 'elements', elements)
                    errors += 1
                    continue
                for k in range(n):
                    b = elements + k * 48
                    width, height, kind, fmt, levels = [struct.unpack_from('<H', data, b + at)[0] for at in (4, 6, 10, 12, 20)]
                    if kind == 1 or not 14 <= fmt <= 16:
                        continue
                    levels += 1
                    pixels_offset, pixels_size = u32(data, b + 24), u32(data, b + 28)
                    face = sum(((max(1, width >> level) + 3) // 4) * ((max(1, height >> level) + 3) // 4) * (8 if fmt == 14 else 16) for level in range(min(levels, 14)))
                    faces = 6 if kind == 2 else 1
                    stride = (face + 127) & ~127 if faces == 6 and version == 5 else face
                    source_size = stride * (faces - 1) + face
                    if not width or not height or width > 4096 or height > 4096 or levels > 13 or pixels_offset + source_size > len(data) or source_size > pixels_size:
                        print('REJECT bitmap', i, k, 'w/h/type/fmt/levels', width, height, kind, fmt, levels, 'offset', pixels_offset, 'declared_pixels', pixels_size, 'required', source_size)
                        errors += 1
                if errors >= 8:
                    break
            raw.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--benchmark', nargs='+', type=Path)
    parser.add_argument('--inspect-stock', action='store_true')
    parser.add_argument('--benchmark-stock', action='store_true',
                        help='Benchmark a10 and d40 using the remote job data root')
    args = parser.parse_args()
    tree = args.tree.resolve()
    if args.inspect_stock:
        inspect_stock()
        return
    if args.benchmark_stock:
        data = Path(os.environ['HALO_REMOTE_ROOT']) / 'data/Documents/maps'
        args.benchmark = [data / 'a10.map', data / 'd40.map']
        if not all(p.is_file() for p in args.benchmark):
            raise FileNotFoundError(f'Stock benchmark maps are missing under {data}')
    with tempfile.TemporaryDirectory(prefix='astc-worker-') as temp:
        build = Path(temp)
        includes = [f'--include-directory={tree / p}' for p in
                    ('port/linux/src', 'port/ios/host', 'port/runtime', 'port/third_party/astcenc', 'port/runtime/include')]
        objects = []
        sources = ['port/linux/src/astc_rewrite.c', 'port/runtime/astc_store.c',
                   'port/ios/host/host_map_identity.m', 'port/ios/tests/astc_worker_probe.m',
                   'port/linux/src/astc_encode.cpp', 'port/ios/host/host_astc_texture.c']
        sources += [str(p.relative_to(tree)) for p in sorted((tree / 'port/third_party/astcenc').glob('*.cpp'))]
        for i, source in enumerate(sources):
            cxx = source.endswith('.cpp')
            command = ['xcrun', 'clang++' if cxx else 'clang', '-O2', '-g',
                       '-DASTCENC_BLOCK_MAX_TEXELS=16', *includes]
            if cxx:
                command += ['-std=c++14']
            if source.endswith('.m'):
                command += ['-fobjc-arc', '-fblocks']
            obj = build / f'{i}.o'
            subprocess.run([*command, '-c', str(tree / source), '--output', str(obj)], check=True)
            objects.append(str(obj))
        executable = build / 'probe'
        subprocess.run(['xcrun', 'clang++', *objects, '-framework', 'Foundation', '-framework',
                        'Metal', '-framework', 'AppKit', '-lz', '--output', str(executable)], check=True)
        root = build / 'data'
        root.mkdir()
        if args.benchmark_stock:
            fixtures(root)
            subprocess.run([str(executable), str(root)], check=True)
            root = build / 'benchmark-data'
            root.mkdir()
        if args.benchmark:
            maps = [str(p.resolve()) for p in args.benchmark]
        else:
            fixtures(root)
            maps = []
        subprocess.run([str(executable), str(root), *maps], check=True)


if __name__ == '__main__':
    main()
