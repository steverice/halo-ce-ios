"""Exercise production match guards with four local controllers and one-view network rosters."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]


def test_generic_match_context_and_ordinary_input_guards(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the benchmark context probe")
    binary = tmp_path / "mp-context"
    subprocess.run([compiler, "-O2", "-fsanitize=address,undefined", "-iquote",
                    str(ROOT / "port/linux/include"), str(ROOT / "port/ios/tests/mp_benchmark_context_probe.c"),
                    str(ROOT / "port/linux/src/mp_benchmark_probe.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
