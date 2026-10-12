#!/usr/bin/env python3
"""Vendor the part of FFmpeg that plays a Bink movie, from an FFmpeg checkout at a release tag.

It configures that checkout for the Bink demuxer and decoders alone (CONFIGURE below: LGPL, no
assembly, no threads), builds its three libraries, links port/ios/tests/bink_probe.c against them
and copies what that link used: the C files of the objects the linker took, every file of the
checkout those were compiled from, and the files configure generated. Nothing is edited.

Usage: refresh.py FFMPEG_CHECKOUT    (run on a Mac with Xcode's command line tools)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LIBRARIES = ('libavformat', 'libavcodec', 'libavutil')
# no --enable-gpl, --enable-version3 or --enable-nonfree: configure reports "LGPL version 2.1 or later"
CONFIGURE = (
    '--disable-everything', '--disable-autodetect', '--disable-programs', '--disable-doc', '--disable-debug',
    '--disable-asm', '--disable-network', '--disable-pthreads', '--disable-avdevice', '--disable-avfilter',
    '--disable-swscale', '--disable-swresample', '--disable-iamf', '--disable-pixelutils',
    '--disable-error-resilience', '--disable-lsp', '--disable-faan',
    '--enable-decoder=bink', '--enable-decoder=binkaudio_rdft', '--enable-decoder=binkaudio_dct',
    '--enable-demuxer=bink',
)
GENERATED = ('config.h', 'config_components.h', 'libavutil/avconfig.h', 'libavutil/ffversion.h')
LICENSES = ('COPYING.LGPLv2.1', 'LICENSE.md')
# what this folder holds besides FFmpeg's files
OURS = ('README.md', 'refresh.py', 'FILES.txt', 'SOURCES.txt', 'VERSION')


def run(*command, cwd):
    subprocess.run([str(part) for part in command], cwd=cwd, check=True)


def output(*command, cwd):
    return subprocess.run([str(part) for part in command], cwd=cwd, check=True, capture_output=True, text=True).stdout


def dependencies(checkout, dependency_file):
    """the files inside the checkout that one object was compiled from"""
    text = dependency_file.read_text().split(':', 1)[1].replace('\\\n', ' ')
    result = set()
    for name in text.split():
        path = Path(name)
        if path.is_absolute():
            if checkout not in path.parents:
                continue
            path = path.relative_to(checkout)
        result.add(Path(os.path.normpath(path)).as_posix())
    return result


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    checkout = Path(sys.argv[1]).resolve()
    tag = output('git', 'describe', '--tags', '--exact-match', cwd=checkout).strip()
    commit = output('git', 'rev-parse', 'HEAD', cwd=checkout).strip()
    if output('git', 'status', '--porcelain', '--untracked-files=no', cwd=checkout).strip():
        sys.exit(f'{checkout} has changes; vendor from a clean checkout of a release tag')
    run('git', 'clean', '--quiet', '--force', '-d', '-x', cwd=checkout)
    run('./configure', *CONFIGURE, cwd=checkout)
    if '#define FFMPEG_LICENSE "LGPL version 2.1 or later"' not in (checkout / 'config.h').read_text():
        sys.exit('configure did not produce an LGPL 2.1 configuration')
    run('make', f'--jobs={os.cpu_count() or 4}', *(f'{library}/{library}.a' for library in LIBRARIES), cwd=checkout)

    with tempfile.TemporaryDirectory() as scratch:
        link_map = Path(scratch) / 'probe.map'
        run('xcrun', 'clang', '-O2', f'-I{checkout}', f'-I{ROOT}/port/runtime', ROOT / 'port/runtime/bink_decoder.c',
            ROOT / 'port/ios/tests/bink_probe.c', *(checkout / library / f'{library}.a' for library in LIBRARIES),
            f'-Wl,-map,{link_map}', '-o', Path(scratch) / 'bink-probe', cwd=checkout)
        # the public headers the decoder itself includes, which no FFmpeg object may need
        decoder_dependencies = Path(scratch) / 'bink_decoder.d'
        run('xcrun', 'clang', '-fsyntax-only', '-MMD', '-MF', decoder_dependencies, f'-I{checkout}',
            f'-I{ROOT}/port/runtime', ROOT / 'port/runtime/bink_decoder.c', cwd=checkout)
        decoder_files = dependencies(checkout, decoder_dependencies)
        objects = sorted(set(re.findall(r'/(lib\w+)/lib\w+\.a(?:\[[^\]]*\])*\((\w+)\.o\)', link_map.read_text(errors='replace'))))
    if not objects:
        sys.exit('the link map names no FFmpeg objects')

    sources, files = [], set(GENERATED) | set(LICENSES) | decoder_files
    for library, name in objects:
        found = [path for path in (checkout / library).rglob(f'{name}.d')]
        if len(found) != 1:
            sys.exit(f'{library}: expected one {name}.d, found {len(found)}')
        source = found[0].with_suffix('.c').relative_to(checkout).as_posix()
        needs = dependencies(checkout, found[0])
        if source not in needs:
            sys.exit(f'{found[0]} does not name {source}')
        sources.append(source)
        files |= needs
    for name in sorted(files - set(LICENSES)):
        text = (checkout / name).read_text(errors='replace')
        if 'GNU General Public License' in text and 'GNU Lesser General Public License' not in text:
            sys.exit(f'{name} is under the GPL, not the LGPL')

    for entry in HERE.iterdir():
        if entry.name in OURS:
            continue
        shutil.rmtree(entry) if entry.is_dir() else entry.unlink()
    for name in sorted(files):
        target = HERE / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(checkout / name, target)
    (HERE / 'SOURCES.txt').write_text(''.join(f'{name}\n' for name in sorted(sources)))
    (HERE / 'FILES.txt').write_text(''.join(f'{name}\n' for name in sorted(files)))
    (HERE / 'VERSION').write_text(f'{tag}\n{commit}\n')
    size = sum((HERE / name).stat().st_size for name in files)
    print(f'{tag}: {len(files)} files ({len(sources)} compiled), {size / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
