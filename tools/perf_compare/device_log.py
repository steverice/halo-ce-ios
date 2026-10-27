"""Read a VISR device run's stderr log and frame log into the sheet's columns for one window.

The frame log (`debug.frame_log`) has one line every ~5 s; the renderer line (`debug.gpu_stats`) every ~10 s and
carries the frame CPU and GPU medians. The two files share no clock, so both are aligned by the guest frame
number: stderr prints `frame N:` lines, and the frame log's `frames=` counts add up to the same sequence. The
alignment is good to one frame-log interval (about 5 s). Ignore the frame log's `level=` field: network-test runs on
the 0.5.1-based build name the wrong level there (`known-bugs.md`).
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
from pathlib import Path

CPU = re.compile(r'frame CPU ([\d.]+) ms, GPU ([\d.]+) ms \(medians; 95th ([\d.]+), ([\d.]+)\)')
SCENE = re.compile(r'halo-linux: scene: (\w+)')
FRAME = re.compile(r'halo-linux: frame (\d+):')
MARKER = 'init.txt: running its commands'
LOG = re.compile(r'^frames time=([\d.]+) scene=(\w+) .*? frames=(\d+) fps=([\d.]+) mean_ms=([\d.]+) p95_ms=([\d.]+) '
                 r'p99_ms=([\d.]+) late=(\d+) .*?size=(\d+x\d+) cap=(\d+) hz=([\d.]+).*?thermal=(\w+)')


def parse_stderr(text: str) -> dict:
    """The frame number at the first level scene and at the init marker, and the renderer lines with their frame."""
    frame, first_scene, marker, cpu = 0, None, None, []
    scene = 'menu'
    for line in text.splitlines():
        found = FRAME.search(line)
        if found:
            frame = int(found.group(1))
        found = SCENE.search(line)
        if found:
            scene = found.group(1)
            if scene != 'menu' and first_scene is None:
                first_scene = frame
        if MARKER in line and marker is None:
            marker = frame
        found = CPU.search(line)
        if found and scene != 'menu':
            cpu.append({'frame': frame, 'cpu_ms': float(found.group(1)), 'gpu_ms': float(found.group(2))})
    return {'first_scene_frame': first_scene, 'marker_frame': marker, 'cpu': cpu}


def parse_frame_log(text: str) -> list[dict]:
    entries, total = [], 0
    for line in text.splitlines():
        found = LOG.match(line)
        if not found:
            continue
        frames = int(found.group(3))
        entries.append({'time': float(found.group(1)), 'scene': found.group(2), 'frames': frames, 'start': total,
                        'mean_ms': float(found.group(5)), 'p95_ms': float(found.group(6)),
                        'p99_ms': float(found.group(7)), 'late': int(found.group(8)), 'size': found.group(9),
                        'cap': found.group(10), 'hz': found.group(11), 'thermal': found.group(12)})
        total += frames
    return entries


def window_columns(window: dict, stderr: str, frame_log: str) -> dict:
    """Sheet columns for the window's sample, or {} with a `note` when the logs do not cover it."""
    info, entries = parse_stderr(stderr), parse_frame_log(frame_log)
    hz = float(entries[-1]['hz']) if entries else 60.0
    # Console windows start at the init marker; the bot's start tick and openings count from the level's start.
    origin = info['marker_frame'] if window['kind'] in ('battle', 'cutscene') else info['first_scene_frame']
    if origin is None or not entries:
        return {'note': 'no window start in the logs'}
    if window['kind'] == 'cutscene':
        first, last = origin, origin + int(window['expected_ticks'] / 30 * hz)
    elif window['kind'] == 'bot':
        first = origin + int((window['start_tick'] / 30) * hz)
        last = first + int(window['ticks'] / 30 * hz)
    else:
        first = origin + int(window['warmup_s'] * hz)
        last = first + int(window['sample_s'] * hz)
    chosen = [e for e in entries if first <= e['start'] + e['frames'] / 2 < last and e['scene'] != 'menu']
    if not chosen:
        return {'note': 'the frame log ends before the window'}
    frames = sum(e['frames'] for e in chosen)
    weight = lambda key: sum(e[key] * e['frames'] for e in chosen) / frames  # noqa: E731
    cpu = [c for c in info['cpu'] if first <= c['frame'] < last + int(10 * hz)]
    late = sum(e['late'] for e in chosen)
    columns = {
        'fps_avg': f'{1000 / weight("mean_ms"):.2f}', 'fps_1pct_low': f'{1000 / weight("p99_ms"):.2f}',
        'p95_ms': f'{weight("p95_ms"):.2f}', 'p99_ms': f'{weight("p99_ms"):.2f}',
        'on_time_pct': f'{100 * (1 - late / frames):.1f}', 'resolution': chosen[0]['size'].replace('x', '×'),
        'cap_fps': chosen[0]['cap'], 'display_hz': f'{float(chosen[0]["hz"]):g}', 'thermal': chosen[-1]['thermal'],
        'frames': frames, 'seconds': round(frames / hz, 1),
    }
    expected = window.get('sample_s') or (window.get('expected_ticks') or window.get('ticks', 0)) / 30
    if columns['seconds'] < 0.9 * expected:
        columns['note'] = f'window shorter than expected ({columns["seconds"]} s of {expected:g} s)'
    if cpu:
        columns['cpu_ms_p50'] = f'{statistics.median(c["cpu_ms"] for c in cpu):.2f}'
        columns['gpu_ms_p50'] = f'{statistics.median(c["gpu_ms"] for c in cpu):.2f}'
    return columns


def main() -> int:
    import windows
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('window')
    parser.add_argument('run_dir', type=Path, help='folder with stderr.log and frame-log.txt')
    args = parser.parse_args()
    table = windows.load()
    print(json.dumps(window_columns(table[args.window], (args.run_dir / 'stderr.log').read_text(errors='replace'),
                                    (args.run_dir / 'frame-log.txt').read_text(errors='replace')), indent=1))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
