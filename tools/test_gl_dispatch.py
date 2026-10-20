"""Keep the stereo overlay's OpenGL calls on the resolved, counted path."""

from pathlib import Path
import re

import pytest


ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("name", ["glUniform4f", "glDeleteProgram"])
def test_stereo_overlay_entry_point_is_resolved_and_counted_on_both_backends(name):
    renderer = (ROOT / "port/linux/src/gpu_gl.c").read_text()
    dispatch = (ROOT / "port/linux/src/gl.h").read_text()

    assert re.search(rf"\b{name}\s*\(", renderer), f"{name} is no longer used by the renderer"
    assert dispatch.count(f"X({name})") == 2, f"{name} must load on OpenGL ES and desktop OpenGL"
    assert dispatch.count(f"#define {name} HALO_GL_COUNTED(halo_{name})") == 2, (
        f"{name} must use the resolved pointer and count the call on both backends"
    )
