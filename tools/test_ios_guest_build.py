import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from tools.ios_guest_build import stable_converter_interpreter


ROOT = Path(__file__).resolve().parents[1]


def test_converter_command_stays_stable_across_venv_aliases(tmp_path):
    if os.name != "posix":
        pytest.skip("the iOS guest converter runs on Apple platforms")
    ninja = shutil.which("ninja")
    if not ninja:
        pytest.skip("requires Ninja")

    base_interpreter = Path(sys.executable).resolve()
    aliases = [tmp_path / "venv-a" / "bin" / "python", tmp_path / "venv-b" / "bin" / "python"]
    for alias in aliases:
        alias.parent.mkdir(parents=True)
        alias.symlink_to(base_interpreter)

    source = tmp_path / "input.s"
    source.write_text(".text\n.globl _probe\n_probe:\n\tmov x0, x1\n\tret\n")
    output = tmp_path / "converted.s"
    converter = ROOT / "tools" / "ios_asm_convert.py"
    build_file = tmp_path / "build.ninja"

    def write_build_file(interpreter, resolve):
        command_interpreter = stable_converter_interpreter(str(interpreter)) if resolve else interpreter
        command = f"{shlex.quote(str(command_interpreter))} {shlex.quote(str(converter))} $in $out"
        build_file.write_text(
            "rule convert\n"
            f"  command = {command}\n"
            "  description = CONVERT $out\n"
            "build converted.s: convert input.s\n"
        )

    def run_ninja():
        return subprocess.run([ninja], cwd=tmp_path, check=True, capture_output=True, text=True).stdout

    # Distinct venv paths make the old command dirty even though both execute the same interpreter.
    write_build_file(aliases[0], resolve=False)
    assert "CONVERT converted.s" in run_ninja()
    assert output.is_file() and "probe:" in output.read_text()

    write_build_file(aliases[1], resolve=False)
    assert "CONVERT converted.s" in run_ninja()

    # The production normalization resolves both aliases to one command; the second graph has no work.
    write_build_file(aliases[0], resolve=True)
    assert "CONVERT converted.s" in run_ninja()
    write_build_file(aliases[1], resolve=True)
    assert "no work to do" in run_ninja().lower()
