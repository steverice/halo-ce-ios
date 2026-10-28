"""Frame timing for OpenGL Mac apps that `metalperftrace` cannot see: `glft.m` is inserted into the app with
`DYLD_INSERT_LIBRARIES` and timestamps each `-[NSOpenGLContext flushBuffer]` / `CGLFlushDrawable` call, the frame
swap, into a memory-mapped file. Intervals between swaps are the same events `metalperftrace` reports for a Metal app
(checked on two Metal ports: identical average rates). The shim only timestamps: it does not read the app's code, and it
never calls `glFinish` here, so it takes no GPU time per frame."""
from __future__ import annotations

import os
from pathlib import Path
import struct
import subprocess
import time

SOURCE = Path(__file__).with_name('glft.m')
HEADER = 8 + 8 + 4 + 4 + 8 * 3  # magic, count, numer, denom, pid, finish, cap


def compile_shim(destination: Path) -> Path:
    """Build and ad hoc sign the shim next to the run's evidence."""
    destination = Path(destination)
    subprocess.run(['clang', '-arch', 'arm64', '-dynamiclib', '-O2', '-fobjc-arc', '-framework', 'AppKit', '-framework', 'OpenGL',
                    '-framework', 'QuartzCore', '-Wno-deprecated-declarations', '-Wno-unused-function', str(SOURCE),
                    '-o', str(destination)], check=True, capture_output=True)
    subprocess.run(['codesign', '--force', '--sign', '-', str(destination)], check=True, capture_output=True)
    return destination


def shim_environment(library: Path, output: Path) -> dict[str, str]:
    return {'DYLD_INSERT_LIBRARIES': str(library), 'GLFT_OUT': str(output)}


def read_swaps(path: Path) -> list[float]:
    """Wall-clock times (seconds) of every recorded frame swap."""
    raw = Path(path).read_bytes()
    count, numer, denom = struct.unpack_from('<QII', raw, 8)
    rows = struct.unpack_from(f'<{2 * count}Q', raw, HEADER)
    offset = time.time() - time.clock_gettime(time.CLOCK_UPTIME_RAW)  # same host and boot as the app
    return [rows[2 * i] * numer / denom / 1e9 + offset for i in range(count)]


def intervals_ms(swaps: list[float], start: float, end: float) -> list[float]:
    """Milliseconds between consecutive swaps that fall inside the window."""
    inside = [t for t in swaps if start <= t <= end]
    return [(b - a) * 1000 for a, b in zip(inside, inside[1:])]


def viewport(path: Path) -> str:
    """The GL viewport at the first swap as `WxH` pixels, from the shim's description file; '' when unreadable."""
    try:
        for line in Path(str(path) + '.txt').read_text().splitlines():
            if line.startswith('viewport '):
                _, _, _, width, height = line.split()
                return f'{width}×{height}'
    except OSError:
        pass
    return ''
