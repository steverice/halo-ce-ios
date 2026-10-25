#!/usr/bin/env python3
"""Measure stock ASTC material gates and save five private comparison crops.

Run through the project's job runner. Output contains game art and must remain
outside the repository. PNG panels: original RGBA, ASTC RGBA, RGB error x8,
original alpha, and ASTC alpha. Rank distinct newly passing textures by their
worst mip/face RGB PSNR; each crop shows that level's highest-error 64x64 patch.
"""
import argparse
import csv
import html
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zlib


BRIDGE_HEADER = r'''
#ifndef ASTC_AUDIT_BRIDGE_H
#define ASTC_AUDIT_BRIDGE_H
#include "astc_encode.h"
struct audit_measurement {
    double psnr, squared_error, channels, coverage_all, coverage_test;
    unsigned alpha_max, crop_x, crop_y, crop_width, crop_height;
    int binary;
    int old_pass, material_pass;
};
#ifdef __cplusplus
extern "C" {
#endif
int audit_measure(int format, const uint8_t *source, unsigned width, unsigned height,
    const uint8_t *encoded, const struct astc_alpha_policy *policy,
    struct audit_measurement *result, uint8_t *crop);
#ifdef __cplusplus
}
#endif
#endif
'''
BRIDGE_SOURCE = r'''
#include "bridge.h"
#include "astc_quality.h"
#include "astcenc.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>
extern "C" void audit_dxt_reference(int, const uint8_t *, unsigned, unsigned, unsigned long *);
extern "C" int audit_measure(int format, const uint8_t *source, unsigned width, unsigned height,
    const uint8_t *encoded, const astc_alpha_policy *policy, audit_measurement *result, uint8_t *crop)
{
    static astcenc_context *context = nullptr;
    if (!context) {
        astcenc_config config;
        if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &config) != ASTCENC_SUCCESS ||
            astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS) return 0;
    }
    size_t texels = size_t(width) * height;
    std::vector<unsigned long> bgra(texels);
    std::vector<uint8_t> original(texels * 4), decoded(texels * 4);
    audit_dxt_reference(format, source, width, height, bgra.data());
    bool binary = true;
    for (size_t i = 0; i < texels; ++i) {
        unsigned long value = bgra[i];
        original[i * 4] = value >> 16; original[i * 4 + 1] = value >> 8;
        original[i * 4 + 2] = value; original[i * 4 + 3] = value >> 24;
        binary &= original[i * 4 + 3] == 0 || original[i * 4 + 3] == 255;
    }
    void *slice = decoded.data();
    astcenc_image image {width, height, 1, ASTCENC_TYPE_U8, &slice};
    const astcenc_swizzle swizzle {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    if (astcenc_decompress_image(context, encoded, size_t((width + 3) / 4) * ((height + 3) / 4) * 16,
        &image, &swizzle, 0) != ASTCENC_SUCCESS) return 0;
    astcenc_decompress_reset(context);
    astc_quality_metrics metrics;
    metrics.add(original.data(), decoded.data(), texels, binary);
    *result = {};
    result->psnr = metrics.squared_error ? 10 * log10(65025.0 * metrics.rgb_channels / metrics.squared_error) : 999;
    result->squared_error = metrics.squared_error; result->channels = metrics.rgb_channels;
    result->binary = binary;
    result->old_pass = metrics.valid(texels, nullptr); result->material_pass = metrics.valid(texels, policy);
    // Count all coverage crossings independently, including binary-alpha levels.
    int64_t deltas[257] = {};
    std::vector<uint64_t> patch_scores(size_t((width + 63) / 64) * ((height + 63) / 64));
    for (size_t i = 0; i < texels; ++i) {
        unsigned a = original[i * 4 + 3], b = decoded[i * 4 + 3];
        unsigned difference = a > b ? a - b : b - a;
        result->alpha_max = std::max(result->alpha_max, difference);
        if (a != b) { ++deltas[std::min(a, b) + 1]; --deltas[std::max(a, b) + 1]; }
        uint64_t score = 0;
        if (a) for (unsigned c = 0; c < 3; ++c) { int d = int(original[i * 4 + c]) - decoded[i * 4 + c]; score += d * d; }
        patch_scores[(i / width / 64) * ((width + 63) / 64) + (i % width / 64)] += score;
    }
    int64_t crossings = 0;
    for (unsigned r = 1; r < 256; ++r) {
        crossings += deltas[r]; double fraction = double(crossings) / texels;
        result->coverage_all = std::max(result->coverage_all, fraction);
        if (!policy || !policy->known || (policy->boundaries[r / 8] & (1u << (r % 8))))
            result->coverage_test = std::max(result->coverage_test, fraction);
    }
    size_t worst = std::max_element(patch_scores.begin(), patch_scores.end()) - patch_scores.begin();
    result->crop_x = (worst % ((width + 63) / 64)) * 64;
    result->crop_y = (worst / ((width + 63) / 64)) * 64;
    result->crop_width = std::min(64u, width - result->crop_x);
    result->crop_height = std::min(64u, height - result->crop_y);
    size_t crop_bytes = size_t(result->crop_width) * result->crop_height * 4;
    for (unsigned y = 0; y < result->crop_height; ++y) {
        size_t at = (size_t(result->crop_y + y) * width + result->crop_x) * 4;
        memcpy(crop + size_t(y) * result->crop_width * 4, original.data() + at, result->crop_width * 4);
        memcpy(crop + crop_bytes + size_t(y) * result->crop_width * 4, decoded.data() + at, result->crop_width * 4);
    }
    return 1;
}
'''

def production_dxt_reference(tree):
    source = (tree / 'port/linux/src/xbox_textures.c').read_text()
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
    return ('#include <stdint.h>\n#define TRUE 1\n#define FALSE 0\ntypedef int BOOL;\n'
            'enum { _texel_dxt1, _texel_dxt3, _texel_dxt5 };\n' + '\n'.join(functions) +
            '\nvoid audit_dxt_reference(int f,const uint8_t *b,unsigned w,unsigned h,unsigned long *p) '
            '{ dxt_decode_level(f,b,w,h,1,p); }\n')


def build_probe(tree, build):
    build.mkdir(parents=True, exist_ok=True)
    (build / 'bridge.h').write_text(BRIDGE_HEADER)
    (build / 'bridge.cpp').write_text(BRIDGE_SOURCE)
    (build / 'reference.c').write_text(production_dxt_reference(tree))
    includes = [f'--include-directory={tree / p}' for p in
                ('port/linux/src', 'port/ios/host', 'port/runtime', 'port/third_party/astcenc', 'port/runtime/include')]
    includes += [f'--include-directory={build}']
    sources = [tree / p for p in ('port/linux/src/astc_rewrite.c', 'port/runtime/astc_store.c',
               'port/ios/host/host_map_identity.m', 'port/ios/tests/astc_alpha_audit.m',
               'port/linux/src/astc_encode.cpp', 'port/ios/host/host_astc_texture.c')]
    sources += sorted((tree / 'port/third_party/astcenc').glob('*.cpp'))
    sources += [build / 'bridge.cpp', build / 'reference.c']
    objects = []
    for i, source in enumerate(sources):
        cxx = source.suffix == '.cpp'
        obj = build / f'{i}.o'
        command = ['xcrun', 'clang++' if cxx else 'clang', '-O2', '-g',
                   '-DASTCENC_BLOCK_MAX_TEXELS=16', *includes]
        if cxx:
            command += ['-std=c++14']
        if source.suffix == '.m':
            command += ['-fobjc-arc', '-fblocks']
        subprocess.run([*command, '-c', str(source), '--output', str(obj)], check=True)
        objects.append(str(obj))
    executable = build / 'audit'
    subprocess.run(['xcrun', 'clang++', *objects, '-framework', 'Foundation', '-framework', 'Metal',
                    '-framework', 'AppKit', '-lz', '--output', str(executable)], check=True)
    return executable


def write_png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    raw = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def render_crops(output):
    manifest = output / 'crops.jsonl'
    if not manifest.is_file():
        return
    gallery = ['<meta charset="utf-8"><title>ASTC alpha audit crops</title><style>body{font:16px system-ui;margin:24px;background:#eee}img{image-rendering:pixelated;max-width:100%}</style>',
               '<h1>Up to five worst distinct newly passing textures</h1><p>Each row: original RGBA, ASTC RGBA, RGB error ×8, original alpha, ASTC alpha. Checkerboard shows transparency. Images are enlarged with nearest-neighbor sampling.</p>']
    for line in manifest.read_text().splitlines():
        entry = json.loads(line)
        w, h = entry['crop']['width'], entry['crop']['height']
        raw = (output / entry['raw_file']).read_bytes()
        original, decoded = raw[:w * h * 4], raw[w * h * 4:]
        scale, gap = max(4, 256 // max(w, h)), 8
        width, height = w * scale * 5 + gap * 4, h * scale
        pixels = bytearray([30] * (width * height * 3))
        for y in range(h):
            for x in range(w):
                at = (y * w + x) * 4
                a, b = original[at:at + 4], decoded[at:at + 4]
                background = 175 if (x // 8 + y // 8) % 2 else 220
                panels = [tuple((v * a[3] + background * (255 - a[3])) // 255 for v in a[:3]),
                          tuple((v * b[3] + background * (255 - b[3])) // 255 for v in b[:3]),
                          tuple(min(255, abs(a[c] - b[c]) * 8) for c in range(3)),
                          (a[3],) * 3, (b[3],) * 3]
                for panel, color in enumerate(panels):
                    for dy in range(scale):
                        for dx in range(scale):
                            dest = ((y * scale + dy) * width + panel * (w * scale + gap) + x * scale + dx) * 3
                            pixels[dest:dest + 3] = bytes(color)
        png = entry['raw_file'].replace('.rgba', '.png')
        write_png(output / png, width, height, pixels)
        caption = (f"{entry['map']} tag {entry['tag']} bitmap {entry['bitmap']}; "
                   f"worst RGB PSNR {entry['min_rgb_psnr']:.6f} dB; "
                   f"face {entry['crop']['face']} mip {entry['crop']['mip']}; "
                   f"crop {w}×{h} at ({entry['crop']['x']}, {entry['crop']['y']})")
        gallery.append(f'<h2>{html.escape(caption)}</h2><img src="{html.escape(png)}">')
    (output / 'index.html').write_text('\n'.join(gallery) + '\n')


def write_instance_gallery(output, rows):
    crops = {entry['key']: entry for entry in
             (json.loads(line) for line in (output / 'crops.jsonl').read_text().splitlines())}
    added = [row for row in rows if not row['direct'] and row['new_pass'] and not row['old_pass']]
    added.sort(key=lambda row: (row['min_rgb_psnr'], row['map'], row['tag'], row['bitmap']))
    gallery = ['<meta charset="utf-8"><title>Five worst ASTC map-texture instances</title>',
               '<style>body{font:16px system-ui;margin:24px;background:#eee}img{image-rendering:pixelated;max-width:100%}</style>',
               '<h1>Five worst newly passing map-texture instances</h1>',
               '<p>Ranked by worst mip/face RGB PSNR. Repeated content is included when it occurs in different maps; duplicate instances use identical comparison pixels. Panels: original RGBA, ASTC RGBA, RGB error ×8, original alpha, ASTC alpha. <a href="index.html">All distinct content crops</a>.</p>']
    instances, seen = [], set()
    for number, row in enumerate(added[:5], 1):
        crop = crops[row['key']]
        original = crop['raw_file'].replace('.rgba', '.png')
        filename = f'instance-{number}.png'
        shutil.copyfile(output / original, output / filename)
        entry = {key: row[key] for key in ('map', 'tag', 'bitmap', 'key', 'min_rgb_psnr',
                 'max_alpha_error', 'test_crossing_fraction')}
        entry.update(crop=crop['crop'], png=filename, source_crop_file=original,
                     duplicate_content=row['key'] in seen)
        seen.add(row['key'])
        instances.append(entry)
        duplicate = ' (duplicate content)' if entry['duplicate_content'] else ''
        caption = (f"{number}. {entry['map']} tag {entry['tag']} bitmap {entry['bitmap']}: "
                   f"{entry['min_rgb_psnr']:.6f} dB{duplicate}")
        gallery.append(f'<h2>{html.escape(caption)}</h2><img src="{filename}">')
    (output / 'five-worst-instances.json').write_text(json.dumps(instances, indent=2) + '\n')
    (output / 'five-worst-instances.html').write_text('\n'.join(gallery) + '\n')


def write_report(output, summary, rows):
    added = [row for row in rows if not row['direct'] and row['new_pass'] and not row['old_pass']]
    lines = ['Encoder-path texture counts; direct rewrites are excluded from these columns.',
             'Map                 Encoder  Old pass/excluded  New pass/excluded  Added',
             '------------------  -------  -----------------  -----------------  -----']
    for row in summary:
        lines.append(f"{row['map']:<18}  {row['encoder']:>7}  {row['old_pass']:>7}/{row['old_excluded']:<9}  "
                     f"{row['new_pass']:>7}/{row['new_excluded']:<9}  {row['newly_passing']:>5}")
    lines += ['', f"Newly passing: {len(added)} map-texture instances, {len({row['key'] for row in added})} distinct content keys.",
              f"Measured all {sum(len(row['levels']) for row in added)} runtime mip/face levels of newly passing textures.",
              f"Worst RGB PSNR: {min((row['min_rgb_psnr'] for row in added), default=999):.6f} dB (source-alpha-zero RGB excluded, matching production).",
              f"Maximum alpha error: {max((row['max_alpha_error'] for row in added), default=0)} byte levels.",
              f"Maximum selected-boundary crossing fraction: {max((row['test_crossing_fraction'] for row in added), default=0):.10f}.",
              'Unknown and continuous alpha blend/mask uses retain the all-boundaries gate.',
              'Five worst map-texture instances: five-worst-instances.html (duplicate content labeled).',
              'Distinct content comparisons: index.html; exact per-level metrics: textures.jsonl.']
    (output / 'report.txt').write_text('\n'.join(lines) + '\n')
    with (output / 'summary.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)


def summarize(output):
    rows = [json.loads(line) for line in (output / 'textures.jsonl').read_text().splitlines()]
    summary = []
    for name in sorted({r['map'] for r in rows}):
        textures = [r for r in rows if r['map'] == name]
        encoder = [r for r in textures if not r['direct']]
        added = [r for r in encoder if r['new_pass'] and not r['old_pass']]
        assert all(r['new_pass'] for r in encoder if r['old_pass']), 'Material policy lost an old pass'
        added_levels = [level for r in added for level in r['levels']]
        for level in added_levels:
            assert level['rgb_psnr'] >= 38, level
            if level['binary_alpha']:
                assert level['max_alpha_error'] <= 8, level
            else:
                assert level['test_crossing_fraction'] <= 0.001, level
        row = {'map': name, 'dxt_textures': len(textures), 'direct': len(textures) - len(encoder),
               'encoder': len(encoder), 'old_pass': sum(r['old_pass'] for r in encoder),
               'old_excluded': sum(not r['old_pass'] for r in encoder),
               'new_pass': sum(r['new_pass'] for r in encoder),
               'new_excluded': sum(not r['new_pass'] for r in encoder), 'newly_passing': len(added),
               'known_policy': sum(r['known_policy'] for r in encoder),
               'newly_passing_mips_faces': sum(len(r['levels']) for r in added),
               'newly_passing_texels': sum(r['texels_measured'] for r in added),
               'worst_added_rgb_psnr': min((r['min_rgb_psnr'] for r in added), default=None),
               'max_added_alpha_error': max((r['max_alpha_error'] for r in added), default=None),
               'max_added_binary_alpha_error': max((level['max_alpha_error'] for level in added_levels if level['binary_alpha']), default=None),
               'max_added_continuous_test_crossing_fraction': max((level['test_crossing_fraction'] for level in added_levels if not level['binary_alpha']), default=None),
               'max_added_test_crossing_fraction': max((r['test_crossing_fraction'] for r in added), default=None),
               'max_added_all_crossing_fraction': max((r['all_crossing_fraction'] for r in added), default=None)}
        summary.append(row)
        print(json.dumps(row, sort_keys=True), flush=True)
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    write_report(output, summary, rows)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--maps', nargs='+', type=Path)
    parser.add_argument('--stock', action='store_true')
    parser.add_argument('--reuse', action='store_true', help='Summarize and render an existing measurement')
    args = parser.parse_args()
    tree, output = args.tree.resolve(), args.output.resolve()
    if output == tree or tree in output.parents:
        parser.error('Output includes game art and must be outside the repository')
    output.mkdir(parents=True, exist_ok=True)
    if not args.reuse:
        maps = args.maps or []
        if args.stock:
            root = Path(os.environ['HALO_REMOTE_ROOT']) / 'data/Documents/maps'
            maps = sorted(root.glob('*.map'))
            maps = [p for p in maps if p.stem not in ('bitmaps', 'sounds', 'loc')]
        if not maps:
            parser.error('Choose --maps or --stock')
        executable = build_probe(tree, output / 'build')
        subprocess.run([str(executable), str(output), *map(str, maps)], check=True)
    rows = summarize(output)
    render_crops(output)
    write_instance_gallery(output, rows)


if __name__ == '__main__':
    main()
