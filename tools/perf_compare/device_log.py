"""Read a VISR device run's stderr log and frame log into the sheet's columns for one window.

The frame log (`debug.frame_log`) has one line every ~5 s; the renderer line (`debug.gpu_stats`) every ~10 s and
carries the frame CPU and GPU medians. The two files share no clock, so both are aligned by the guest frame
number: stderr prints `frame N:` lines, and the frame log's `frames=` counts add up to the same sequence. The
alignment is good to one frame-log interval (about 5 s). Windows are placed in seconds on the frame log's clock. Ignore the frame log's `level=` field: network-test runs on
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
            cpu.append({'frame': frame, 'cpu_ms': float(found.group(1)), 'gpu_ms': float(found.group(2)),
                        'cpu_p95': float(found.group(3)), 'gpu_p95': float(found.group(4))})
    return {'first_scene_frame': first_scene, 'marker_frame': marker, 'cpu': cpu}


def parse_frame_log(text: str) -> list[dict]:
    """Frame-log lines with a time axis: `t0` and `t1` are session seconds, `start` the frames before the entry."""
    entries, total, prev = [], 0, 0.0
    for line in text.splitlines():
        found = LOG.match(line)
        if not found:
            continue
        frames, end = int(found.group(3)), float(found.group(1))
        if not entries:
            prev = max(0.0, end - frames * float(found.group(5)) / 1000)
        entries.append({'time': end, 't0': prev, 't1': end, 'scene': found.group(2), 'frames': frames, 'start': total,
                        'mean_ms': float(found.group(5)), 'p95_ms': float(found.group(6)),
                        'p99_ms': float(found.group(7)), 'late': int(found.group(8)), 'size': found.group(9),
                        'cap': found.group(10), 'hz': found.group(11), 'thermal': found.group(12)})
        total += frames
        prev = end
    return entries


def time_at_frame(entries: list[dict], frame: float) -> float | None:
    """Session second of a guest frame number, interpolated inside the frame-log entry that holds it."""
    for entry in entries:
        if entry['start'] <= frame < entry['start'] + entry['frames']:
            return entry['t0'] + (entry['t1'] - entry['t0']) * (frame - entry['start']) / max(entry['frames'], 1)
    return None


def window_columns(window: dict, stderr: str, frame_log: str) -> dict:
    """Sheet columns for the window's sample, or {} with a `note` when the logs do not cover it.

    The window is placed on the frame log's clock, in seconds. A device that runs below its display rate (the Apple TV
    HD at 30 fps, a battle at 12 fps) has fewer frames per second than `hz`, so converting seconds to frames with the
    display rate would put the window in the wrong place. The init marker is a guest frame number (stderr prints one
    every 60 frames), converted to seconds through the frame log's cumulative frame counts.
    """
    info, entries = parse_stderr(stderr), parse_frame_log(frame_log)
    if not entries:
        return {'note': 'no window start in the logs'}
    marker = None if info['marker_frame'] is None else time_at_frame(entries, info['marker_frame'] + 30)
    first_level = next((e for e in entries if e['scene'] != 'menu'), None)
    kind = window['kind']
    if kind in ('battle', 'cutscene', 'bot') and marker is None or kind == 'opening' and first_level is None:
        return {'note': 'no window start in the logs'}
    # Console windows start at the init marker; openings count from the level's first frames.
    if kind == 'cutscene':
        first, last = marker, marker + window['expected_ticks'] / 30
    elif kind == 'bot':
        first = marker + window['start_tick'] / 30
        last = first + window['ticks'] / 30
    elif kind == 'battle':
        first = marker + window['warmup_s']
        last = first + window['sample_s']
    else:
        first = first_level['t0'] + window['warmup_s']
        last = first + window['sample_s']
    chosen = [e for e in entries if first <= (e['t0'] + e['t1']) / 2 < last and e['scene'] != 'menu' and e['frames'] > 10]
    if not chosen:
        return {'note': 'the frame log ends before the window'}
    frames = sum(e['frames'] for e in chosen)
    seconds = sum(e['t1'] - e['t0'] for e in chosen)
    weight = lambda key: sum(e[key] * e['frames'] for e in chosen) / frames  # noqa: E731
    late = sum(e['late'] for e in chosen)
    columns = {
        'fps_avg': f'{1000 / weight("mean_ms"):.2f}', 'fps_1pct_low': f'{1000 / weight("p99_ms"):.2f}',
        'p95_ms': f'{weight("p95_ms"):.2f}', 'p99_ms': f'{weight("p99_ms"):.2f}',
        'on_time_pct': f'{100 * (1 - late / frames):.1f}', 'resolution': chosen[0]['size'].replace('x', '×'),
        'cap_fps': chosen[0]['cap'], 'display_hz': f'{float(chosen[0]["hz"]):g}', 'thermal': chosen[-1]['thermal'],
        'frames': frames, 'seconds': round(seconds, 1),
    }
    expected = window.get('sample_s') or (window.get('expected_ticks') or window.get('ticks', 0)) / 30
    if columns['seconds'] < 0.9 * expected:
        columns['note'] = f'window shorter than expected ({columns["seconds"]} s of {expected:g} s)'
    # A renderer line covers the frames since the one before it; it belongs to the window when the middle is inside.
    cpu, previous = [], 0
    for line in info['cpu']:
        at = time_at_frame(entries, (previous + line['frame']) / 2)
        previous = line['frame']
        if at is not None and first <= at < last:
            cpu.append(line)
    if cpu:
        columns['cpu_ms_p50'] = f'{statistics.median(c["cpu_ms"] for c in cpu):.2f}'
        columns['gpu_ms_p50'] = f'{statistics.median(c["gpu_ms"] for c in cpu):.2f}'
        columns['cpu_ms_p95'] = f'{max(c["cpu_p95"] for c in cpu):.1f}'
        columns['gpu_ms_p95'] = f'{max(c["gpu_p95"] for c in cpu):.1f}'
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
