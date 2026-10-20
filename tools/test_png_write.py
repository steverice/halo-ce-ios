"""Tests for the screenshot PNG writer (port/linux/src/png_write.h) and the readers of it in tools/mac_run.py.

The header is compiled into a probe that writes pictures of several shapes; Python's zlib and mac_run.read_image
must read them back to the same BGRA pixels, and a PNG must compare equal to an older run's BMP of the same frame.
"""
from pathlib import Path
import random
import shutil
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mac_run  # noqa: E402
from test_mac_run import _bmp, _result  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

PROBE = r'''
#include "png_write.h"
int main(int argc, char **argv)
{
	unsigned long width = strtoul(argv[2], 0, 10), height = strtoul(argv[3], 0, 10);
	unsigned char *pixels = malloc(width * height * 4);
	FILE *in = argc == 5 ? fopen(argv[4], "rb") : NULL;

	if (!in || fread(pixels, 1, width * height * 4, in) != width * height * 4)
		return 2;
	return !png_write_bgra(argv[1], pixels, width, height, width * 4);
}
'''

pytestmark = pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    folder = tmp_path_factory.mktemp("png_probe")
    (folder / "probe.c").write_text(PROBE)
    subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "port/linux/src"),
                    "-o", str(folder / "probe"), str(folder / "probe.c")], check=True)
    return folder


def _write(probe, name, width, height, pixels):
    (probe / f"{name}.raw").write_bytes(pixels)
    out = probe / f"{name}.png"
    subprocess.run([str(probe / "probe"), str(out), str(width), str(height), str(probe / f"{name}.raw")], check=True)
    return out.read_bytes()


def _opaque(pixels):
    data = bytearray(pixels)
    data[3::4] = b"\xff" * (len(data) // 4)
    return bytes(data)


@pytest.mark.parametrize("name,width,height", [("one", 1, 1), ("wide", 257, 3), ("tall", 3, 300), ("big", 640, 360)])
def test_png_round_trips_noise_and_flat_areas(probe, name, width, height):
    rng = random.Random(width * 1000 + height)
    pixels = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            at = (y * width + x) * 4
            pixels[at:at + 4] = bytes(rng.randrange(256) for _ in range(3)) + b"\xff" if (x * y) % 7 == 0 else \
                bytes([x & 255, y & 255, 40, 255])
    data = _write(probe, name, width, height, bytes(pixels))
    assert mac_run.read_image(data) == (width, height, bytes(pixels))


def test_png_drops_alpha_and_is_much_smaller_than_the_bmp(probe):
    pixels = bytes([10, 20, 30, 7]) * (400 * 300)
    data = _write(probe, "flat", 400, 300, pixels)
    assert mac_run.read_image(data) == (400, 300, _opaque(pixels))
    assert len(data) < len(pixels) // 50


def test_compare_matches_a_png_with_an_older_bmp(tmp_path):
    pixels = b"\x01\x02\x03\xff\x04\x05\x06\xff"
    a = _result(tmp_path / "a", {}, {"frame00300.bmp": _bmp(2, 1, pixels)}, "")
    b = _result(tmp_path / "b", {}, {}, "")
    # a stored (uncompressed) PNG of the same pixels, as any encoder may write
    import struct
    import zlib

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    rows = b"\x00" + bytes([3, 2, 1, 6, 5, 4])
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 1, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    (tmp_path / "b" / "runner/shots").mkdir(parents=True, exist_ok=True)
    (tmp_path / "b" / "runner/shots" / "frame00300.png").write_bytes(png)
    assert mac_run.read_image(png) == (2, 1, pixels)
    assert not any("frame00300" in p for p in mac_run.compare(a, b))
