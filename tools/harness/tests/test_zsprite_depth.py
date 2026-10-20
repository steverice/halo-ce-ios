"""Check the actual pixel-shader translator's DOT_ZW output for GL and Metal."""

from pathlib import Path
import subprocess
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[3]
PROBE = r'''
#include "xgpu.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...) { (void)format; }
void nv2a_uniform_declarations(struct xgpu_text *text, const struct nv2a_dialect *dialect, int stage)
{
    (void)text; (void)dialect; (void)stage;
}

static void check(const struct nv2a_dialect *dialect, int dot_zw)
{
    struct nv2a_pixel_shader_key key = {0};
    char *shader;

    if (dot_zw)
        key.texture_modes = 0x0aUL << (5 * 2);
    shader = nv2a_pixel_shader_translate(dialect, &key);
    assert(shader);
    if (dot_zw)
    {
        assert(strstr(shader, "replaced_depth = dot2 > 0.0 ? clamp(dot1 / dot2 / 16777215.0") != NULL);
        if (dialect->msl)
        {
            assert(strstr(shader, "float depth [[depth(any)]]") != NULL);
            assert(strstr(shader, "output.depth = 1.0 - replaced_depth") != NULL);
            assert(strstr(shader, "gl_FragDepth") == NULL);
        }
        else
            assert(strstr(shader, "gl_FragDepth = replaced_depth") != NULL);
    }
    else
    {
        assert(strstr(shader, "replaced_depth") == NULL);
        assert(strstr(shader, "gl_FragDepth") == NULL);
        assert(strstr(shader, "FragmentWithDepth") == NULL);
    }
    free(shader);
}

int main(void)
{
    struct nv2a_dialect gl = {.version = 450};
    struct nv2a_dialect metal = {.version = GPU_SHADER_LANGUAGE_MSL, .msl = 1};

    check(&gl, 0);
    check(&gl, 1);
    check(&metal, 0);
    check(&metal, 1);
    return 0;
}
'''


@pytest.mark.skipif(sys.platform != "linux", reason="the production translator uses the 32-bit Linux ABI")
def test_dot_zw_depth_generation_for_gl_and_metal():
    objects = [ROOT / "build/linux/obj/port/linux/src" / name for name in
               ("nv2a_psh.o", "nv2a_msl.o", "xgpu_text.o")]
    assert all(path.exists() for path in objects), "build the Linux release before this test"
    with tempfile.TemporaryDirectory(prefix="zsprite-depth-") as directory:
        program = Path(directory, "probe.c")
        binary = Path(directory, "probe")
        program.write_text(PROBE)
        subprocess.run([
            "clang", "--target=i686-linux-gnu", "-m32", "-no-pie",
            "-std=gnu11", "-fms-extensions", "-fshort-wchar", "-malign-double",
            "-DHALO_LINUX_PLATFORM_LAYER",
            "-Wall", "-Wextra",
            "-Wno-macro-redefined", "-Wno-ignored-attributes",
            "-include", "port/linux/include/halo_linux_prefix.h",
            "-include", "build/linux/platform_msvc_semantics.h",
            "-Iport/linux/src", "-Iport/linux/include",
            "-idirafter", "port/include/xdk",
            str(program), *(str(path) for path in objects), "-o", str(binary),
        ], cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True)
