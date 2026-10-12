#!/usr/bin/env python3
"""Build the Bink decoder for this Mac and decode the player's own intro movie with it.

The vendored FFmpeg sources (port/third_party/ffmpeg-bink), port/runtime/bink_decoder.c and
port/ios/tests/bink_probe.c are compiled natively, and the probe decodes every frame of the
movie: its size, frame count and audio length must be what its header says, every pixel
opaque, and, for the NTSC disc's intro, which this was written against, the numbers and the
pictures' hash below.

The movie is game content and is never in the repository. It comes from --movie, or is read
out of the disc image named by --xiso, or else is looked for where the Mac runner keeps the
game (tools/mac_run.py). With --if-present, a Mac without the game only builds the decoder.
"""
import argparse
import concurrent.futures
import glob
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FFMPEG = ROOT / 'port/third_party/ffmpeg-bink'
BUILD = ROOT / 'build/bink-test'
FLAGS = ['-std=c17', '-O2', '-w', '-DHAVE_AV_CONFIG_H', '-D_ISOC11_SOURCE', '-fno-math-errno', '-fno-signed-zeros',
         f'-I{FFMPEG}']
SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
# where a whole-disc image has its game partition (port/runtime/xiso.c)
PARTITIONS = (0, 0x0FD90000, 0x02080000, 0x18300000)
# intro.bik by its size in bytes: what the probe must print for it
KNOWN = {
    10727656: dict(width=640, height=480, frames=481, rate='2997/100', audio_rate=44100, audio_channels=2,
                   audio_frames=708480, picture_hash='1188bc5a4fafffc0'),
}


def compile_one(source, obj, flags):
    if obj.exists() and obj.stat().st_mtime >= source.stat().st_mtime:
        return
    obj.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['xcrun', 'clang', *flags, '-c', str(source), '-o', str(obj)], check=True)


def build():
    """compiles the decoder and the probe; returns the probe's path"""
    jobs = []
    for name in (FFMPEG / 'SOURCES.txt').read_text().split():
        library = name.split('/')[0][3:]
        jobs.append((FFMPEG / name, BUILD / 'ffmpeg' / Path(name).with_suffix('.o'), FLAGS + [f'-DBUILDING_{library}']))
    for name in ('port/runtime/bink_decoder.c', 'port/ios/tests/bink_probe.c'):
        jobs.append((ROOT / name, BUILD / Path(name).with_suffix('.o').name,
                     ['-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{FFMPEG}', f'-I{ROOT}/port/runtime']))
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for future in [pool.submit(compile_one, *job) for job in jobs]:
            future.result()
    probe = BUILD / 'bink-probe'
    subprocess.run(['xcrun', 'clang', *(str(obj) for _, obj, _ in jobs), '-o', str(probe)], check=True)
    return probe


def xiso_entries(image, base, sector, size):
    """a directory's entries: {lowercase name: (sector, size)}"""
    image.seek(base + sector * SECTOR)
    table, result, pending, seen = image.read(size), {}, [0], set()
    while pending:
        offset = pending.pop()
        if offset in seen or offset + 14 > len(table):
            continue
        seen.add(offset)
        left, right, start, length, _, count = struct.unpack_from('<HHIIBB', table, offset)
        if left == 0xffff:
            continue
        result[table[offset + 14:offset + 14 + count].decode('ascii', 'replace').lower()] = (start, length)
        pending += [link * 4 for link in (left, right) if link]
    return result


def xiso_read(path, name):
    """the bytes of the file `name` (folder/file) in the disc image at path, or None"""
    with open(path, 'rb') as image:
        for base in PARTITIONS:
            image.seek(base + 0x10000)
            header = image.read(SECTOR)
            if header[:20] == MAGIC and header[0x7ec:0x7ec + 20] == MAGIC:
                break
        else:
            return None
        sector, size = struct.unpack_from('<II', header, 20)
        folder, file = name.split('/')
        entries = xiso_entries(image, base, sector, size)
        if folder not in entries:
            return None
        entries = xiso_entries(image, base, *entries[folder])
        if file not in entries:
            return None
        image.seek(base + entries[file][0] * SECTOR)
        return image.read(entries[file][1])


def find_movie(args, scratch):
    """the intro's path, read out of a disc image into scratch if need be; None if the game is not here"""
    if args.movie:
        return args.movie
    # the native runner's data folder and the iPad runner's container (tools/mac_run.py)
    folders = [Path(os.environ['HALO_NATIVE_DATA'])] if os.environ.get('HALO_NATIVE_DATA') else []
    folders += [Path(path) for path in glob.glob(str(Path.home() / 'Library/Application Support/*/runner-data'))]
    documents = [Path(path) for path in glob.glob(str(Path.home() / 'Library/Containers/*/Data/Documents'))]
    for folder in folders + documents:
        if (folder / 'bink/intro.bik').is_file():
            return folder / 'bink/intro.bik'
    images = [args.xiso] if args.xiso else sorted(image for folder in documents for image in folder.glob('*.iso'))
    for image in images:
        data = xiso_read(image, 'bink/intro.bik')
        if data:
            movie = Path(scratch) / 'intro.bik'
            movie.write_bytes(data)
            return movie
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--movie', type=Path, help='a Bink movie to decode (default: the game\'s intro)')
    parser.add_argument('--xiso', type=Path, help='the disc image to read bink/intro.bik from')
    parser.add_argument('--if-present', action='store_true', help='without the game, build the decoder and stop')
    parser.add_argument('--frame', type=int, help='also write this frame (from 0) as a picture')
    parser.add_argument('--out', type=Path, help='where --frame\'s PPM picture goes')
    args = parser.parse_args()
    if (args.frame is None) != (args.out is None):
        parser.error('--frame and --out go together')
    probe = build()
    with tempfile.TemporaryDirectory() as scratch:
        movie = find_movie(args, scratch)
        if not movie:
            if args.if_present:
                print('bink_test: the decoder builds; no game files here, so nothing was decoded')
                return
            sys.exit('bink_test: no movie found; name one with --movie, or the disc image with --xiso')
        command = [str(probe), str(movie)] + ([str(args.frame), str(args.out)] if args.out else [])
        text = subprocess.run(command, check=True, capture_output=True, text=True).stdout
        size = movie.stat().st_size
    found = dict(re.findall(r'(\w+)=(\S+)', text))
    print(text.strip())
    problems = []
    if found['decoded_frames'] != found['frames']:
        problems.append(f'decoded {found["decoded_frames"]} of {found["frames"]} frames')
    if found['opaque_errors'] != '0':
        problems.append(f'{found["opaque_errors"]} pixels are not opaque')
    numerator, denominator = (int(part) for part in found['rate'].split('/'))
    if int(found['audio_channels']):
        seconds = int(found['frames']) * denominator / numerator
        audio_seconds = int(found['audio_frames']) / int(found['audio_rate'])
        if abs(audio_seconds - seconds) > 0.5:
            problems.append(f'{audio_seconds:.2f} s of audio for {seconds:.2f} s of video')
    for key, value in KNOWN.get(size, {}).items():
        if found[key] != str(value):
            problems.append(f'{key} is {found[key]}, expected {value}')
    if problems:
        sys.exit('bink_test: ' + '; '.join(problems))
    print(f'bink_test: ok ({"the known intro" if size in KNOWN else "header checks only: not a movie this test knows"})')


if __name__ == '__main__':
    main()
