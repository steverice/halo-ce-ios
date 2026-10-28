from pathlib import Path
import struct
import sys
import time

import pytest

sys.path.insert(0, str(Path(__file__).parent / 'perf_compare'))
import adapters  # noqa: E402
import glft  # noqa: E402
import results  # noqa: E402


def write_log(path, swaps_ns, numer=1, denom=1):
    header = struct.pack('<8sQIIQQQ', b'GLFT0001', len(swaps_ns), numer, denom, 1, 0, 1 << 22)
    body = b''.join(struct.pack('<QQ', t, 0) for t in swaps_ns)
    path.write_bytes(header + body)


def test_header_size_matches_the_shim_layout():
    assert glft.HEADER == struct.calcsize('<8sQIIQQQ')


def test_swaps_become_wall_clock_times_and_intervals_inside_the_window(tmp_path):
    base = time.clock_gettime_ns(time.CLOCK_UPTIME_RAW)
    step = 5_000_000  # 5 ms between swaps
    write_log(tmp_path / 'glft.bin', [base + i * step for i in range(200)])
    swaps = glft.read_swaps(tmp_path / 'glft.bin')
    assert len(swaps) == 200 and all(b > a for a, b in zip(swaps, swaps[1:]))
    window = glft.intervals_ms(swaps, swaps[50], swaps[150])
    assert len(window) == 100 and all(v == pytest.approx(5.0, abs=1e-3) for v in window)
    metrics = results.metrics(window, 60)
    assert metrics['fps'] == pytest.approx(200.0, rel=1e-3)


def test_mach_ticks_use_the_recorded_timebase(tmp_path):
    base = time.clock_gettime_ns(time.CLOCK_UPTIME_RAW)
    # Apple silicon's 125/3 timebase: 24,000 ticks are exactly one millisecond.
    write_log(tmp_path / 'glft.bin', [base * 3 // 125 + i * 24_000 for i in range(5)], numer=125, denom=3)
    intervals = glft.intervals_ms(glft.read_swaps(tmp_path / 'glft.bin'), 0, float('inf'))
    assert intervals == pytest.approx([1.0] * 4, rel=1e-3)


def test_viewport_is_read_from_the_description_file(tmp_path):
    (tmp_path / 'glft.bin.txt').write_text('viewport 0 0 2560 1920\nrenderer Apple M4\n')
    assert glft.viewport(tmp_path / 'glft.bin') == '2560×1920'
    assert glft.viewport(tmp_path / 'missing.bin') == ''


def test_opengl_ports_use_the_swap_timer_and_the_uncap_environment():
    assert adapters.timing_method({'id': 'chupathingy'}, {'native_method': 'metalperftrace'}) == 'glft'
    assert adapters.timing_method({'id': 'visr'}, {'native_method': 'metalperftrace'}) == 'metalperftrace'
    assert adapters.GLFT_ENVIRONMENT['uncapped'] == {'HALO_NO_VSYNC': '1', 'HALO_MAX_FPS': '-1'}
    env = glft.shim_environment(Path('/x/libglft.dylib'), Path('/x/glft.bin'))
    assert env == {'DYLD_INSERT_LIBRARIES': '/x/libglft.dylib', 'GLFT_OUT': '/x/glft.bin'}


def test_swap_method_label_has_its_own_group():
    assert results.method_group({'method': 'OpenGL frame-swap timestamps (glft)'}) == 'swap'
    assert results.method_group({'method': 'metalperftrace on-glass intervals'}) == 'trace'
    assert 'swap' in results.METHOD_NOTES
