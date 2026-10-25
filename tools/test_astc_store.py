"""Portable durable ASTC store regression probe."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class AstcStoreTests(unittest.TestCase):
    def test_crash_resume_and_pinned_views(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("A C compiler is required")
        with tempfile.TemporaryDirectory(prefix="astc-store-probe-") as temp:
            executable = pathlib.Path(temp) / "probe"
            subprocess.run([
                compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                "-Werror", "-pthread", str(ROOT / "port/ios/tests/astc_store_probe.c"),
                str(ROOT / "port/runtime/astc_store.c"), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
