"""tools/render_replay.py and tools/gpu_interface_hash.py: a capture's header, staleness and frame comparison"""
import gzip
import struct

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import gpu_interface_hash, render_replay  # noqa: E402


def capture_bytes(interface_hash=0x11, front_end_hash=0x22, shader_language=1, first=600, count=30, label=b"metal a10"):
    capabilities = struct.pack("<8BH6BI", 1, 1, 0, 0, 0, 0, 1, 1, shader_language, 0, 0, 0, 0, 0, 0, 4096)
    return render_replay.HEADER.pack(render_replay.MAGIC, 1, interface_hash, front_end_hash, 1, 2, 3, 4, 5, 6, 0x1002,
                                     capabilities, first, count, b"ab12", label)


def test_header_fields(tmp_path):
    path = tmp_path / "a.capture"
    with gzip.open(path, "wb") as file:
        file.write(capture_bytes() + b"\0" * 64)
    header = render_replay.read_header(path)
    assert header["backend"] == "metal" and header["first_frame"] == 600 and header["frame_count"] == 30
    assert header["label"] == "metal a10" and header["guest_sha256"] == "ab12" and header["interface_hash"] == 0x11
    with gzip.open(path, "wb") as file:
        file.write(capture_bytes(shader_language=300))
    assert render_replay.read_header(path)["backend"] == "gl"


def test_header_refuses_other_files(tmp_path):
    path = tmp_path / "b.capture"
    with gzip.open(path, "wb") as file:
        file.write(b"something else entirely" * 20)
    try:
        render_replay.read_header(path)
    except ValueError as error:
        assert "not a render capture" in str(error)
    else:
        raise AssertionError("a file without the magic was read as a capture")


def test_staleness():
    header = {"interface_hash": 0x11, "front_end_hash": 0x22}
    assert render_replay.staleness(header, (0x11, 0x22)) is None
    assert render_replay.staleness(header, (0x12, 0x22)) == "interface"
    assert render_replay.staleness(header, (0x11, 0x23)) == "front end"


def test_interface_hash_ignores_comments_and_spacing(tmp_path):
    (tmp_path / "a.h").write_text("/* one */\nstruct s { int a; };\n")
    first = gpu_interface_hash.source_hash(("a.h",), tmp_path)
    (tmp_path / "a.h").write_text("/* another\ncomment */\nstruct s\n{\n\tint a; // the field\n};\n")
    assert gpu_interface_hash.source_hash(("a.h",), tmp_path) == first
    (tmp_path / "a.h").write_text("struct s { int a; int b; };\n")
    assert gpu_interface_hash.source_hash(("a.h",), tmp_path) != first


def bmp(pixels, width, height):
    header = bytearray(54)
    header[:2] = b"BM"
    struct.pack_into("<I", header, 10, 54)
    struct.pack_into("<ii", header, 18, width, -height)
    struct.pack_into("<H", header, 28, 32)
    return bytes(header) + pixels


def test_compare_frames(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    a.mkdir()
    b.mkdir()
    (a / "frames.txt").write_text("frame00600 2x1 00000000000000aa\nframe00601 2x1 00000000000000bb\n")
    (b / "frames.txt").write_text("frame00600 2x1 00000000000000aa\nframe00601 2x1 00000000000000bb\n")
    assert render_replay.compare_frames(a, b) == []
    (b / "frames.txt").write_text("frame00600 2x1 00000000000000aa\nframe00601 2x1 00000000000000cc\n")
    (a / "frame00601.bmp").write_bytes(bmp(bytes([10, 10, 10, 255, 20, 20, 20, 255]), 2, 1))
    (b / "frame00601.bmp").write_bytes(bmp(bytes([10, 10, 10, 255, 20, 23, 20, 255]), 2, 1))
    problems = render_replay.compare_frames(a, b)
    assert len(problems) == 1 and "frame00601: 1 pixels differ" in problems[0]
    # within a tolerance (GL against Metal)
    assert render_replay.compare_frames(a, b, channel_tolerance=3) == []
    (b / "frames.txt").write_text("frame00600 2x1 00000000000000aa\n")
    assert "2 frames expected, 1 drawn" in render_replay.compare_frames(a, b)[0]
    assert render_replay.compare_frames(tmp_path / "missing", b) == [f"no frames.txt in {tmp_path / 'missing'}"]


if __name__ == "__main__":
    # tools/ios_test.py runs this file as a script: the job hosts' python has no pytest
    for name, test in sorted(globals().items()):
        if name.startswith("test_"):
            with tempfile.TemporaryDirectory() as folder:
                test(Path(folder)) if test.__code__.co_argcount else test()
    print("render replay tool tests passed")
