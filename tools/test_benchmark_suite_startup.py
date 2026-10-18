"""Run the production benchmark state machine and typed match handoff without game data."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]


def test_benchmark_startup_and_typed_multiplayer_handoff(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the benchmark startup probe")
    binary = tmp_path / "benchmark-startup-probe"
    subprocess.run([compiler, "-O2", "-fsanitize=address,undefined",
                    "-I", str(ROOT / "port/linux/src"), "-iquote", str(ROOT / "port/linux/include"),
                    str(ROOT / "port/ios/tests/benchmark_startup_probe.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
