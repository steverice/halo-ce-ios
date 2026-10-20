"""Exercise the real contrail renderer's unsupported-type branch with allocated buffers."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, enum_with, function, read, run  # noqa: E402

SOURCE = "source/render/render_contrails.c"
CASES = ("ground", "unknown", "repeat-then-supported")


def generated(fault=None):
    source = read(SOURCE)
    code = function(source, "render_contrail")
    drawn_point = function(source, "contrail_point_drawn")
    if fault:
        branch = code.index("case _contrail_render_type_ground:")
        end = code.index("\t\t\t\t\treturn;", branch)
        call = "rasterizer_dynamic_triangles_unlock(triangle_buffer_index);"
        assert code[branch:end].count(call) == 1
        code = code[:branch] + code[branch:end].replace(call, "/* missing triangle unlock */", 1) + code[end:]
    types = enum_with(read("source/effects/contrail_definitions.h"), "_contrail_render_type_ground")
    return (("config.inc", types + "\n"), ("under_test.inc", drawn_point + "\n" + code + "\n"))


def build_native(generated_files):
    """The cleanup branch has no ABI layout dependency; macOS hosts lack a 32-bit C runtime."""
    work = Path(tempfile.mkdtemp(prefix="contrail-cleanup-"))
    for name, contents in generated_files:
        (work / name).write_text(contents)
    executable = work / "contrail_cleanup"
    root = Path(__file__).resolve().parents[3]
    command = [os.environ.get("CC", "clang"), "-std=gnu99", "-O2", "-Wall", "-Werror",
               "-Wno-unused-function", "-Wno-unused-variable", "-I", str(root / "tools/harness/include"),
               "-I", str(work), str(Path(__file__).with_name("contrail_cleanup.c")), "-o", str(executable), "-lm"]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return executable


class TestContrailCleanup(unittest.TestCase):
    def test_cleanup(self):
        executable = build_native(generated())
        for case in CASES:
            with self.subTest(case=case):
                status, output = run(executable, case)
                self.assertEqual(status, 0, output)

    def test_missing_cleanup_detected(self):
        status, output = run(build_native(generated(True)), "ground")
        self.assertEqual(status, CHECK_FAILED, output)
