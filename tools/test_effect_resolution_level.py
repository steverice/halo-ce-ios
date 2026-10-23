"""`effect_resolution_level` (port/linux/src/d3d8_device.c) reads display.effect_resolution's three values, and the old
boolean's "true" and "false" from the environment or a --set as "half" and "xbox"."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def _function():
    source = (ROOT / "port/linux/src/d3d8_device.c").read_text()
    match = re.search(r"static int effect_resolution_level\(const char \*value\)\n\{.*?\n\}\n", source, re.S)
    assert match, "effect_resolution_level not found"
    return match.group(0)


@pytest.mark.skipif(not shutil.which("cc"), reason="needs a C compiler")
def test_effect_resolution_levels(tmp_path):
    program = tmp_path / "level.c"
    program.write_text(
        "#include <stdio.h>\n#include <string.h>\n#include <stdarg.h>\n"
        "static int logged;\n"
        "static void platform_log(const char *format, ...) { (void)format; logged++; }\n"
        + _function() +
        "int main(void) {\n"
        "    const char *values[] = { \"xbox\", \"half\", \"full\", \"true\", \"false\", \"bogus\" };\n"
        "    for (int i = 0; i < 6; i++) { logged = 0; printf(\"%s %d %d\\n\", values[i], effect_resolution_level(values[i]), logged); }\n"
        "    return 0;\n}\n")
    binary = tmp_path / "level"
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(program), "-o", str(binary)], check=True)
    lines = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.split("\n")
    assert lines[:6] == ["xbox 0 0", "half 1 0", "full 2 0", "true 1 1", "false 0 1", "bogus 1 1"]
