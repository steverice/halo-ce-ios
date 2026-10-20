"""Check the actual pixel-shader translator's DOT_ZW output for GL and Metal."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[3]
PROBE = r'''
#include "xgpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...) { (void)format; }

static void check(const struct nv2a_dialect *dialect, int dot_zw, int count_samples)
{
    struct nv2a_pixel_shader_key key = {0};
    char *shader;

    if (dot_zw)
        key.texture_modes = 0x0aUL << (5 * 2);
    key.count_samples = count_samples;
    shader = nv2a_pixel_shader_translate(dialect, &key);
    assert(shader);
    if (dot_zw && !(dialect->es && count_samples))
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
    if (count_samples)
    {
        assert(strstr(shader, "layout(early_fragment_tests) in;") != NULL);
        assert(strstr(shader, "atomicCounterIncrement(visible_samples)") != NULL);
    }
    free(shader);
}

static void write_es(const char *path, int count_samples)
{
    struct nv2a_dialect es = {.version = 310, .es = 1};
    struct nv2a_pixel_shader_key key = {0};
    FILE *file;
    char *shader;

    key.texture_modes = 0x0aUL << (5 * 2);
    key.count_samples = count_samples;
    shader = nv2a_pixel_shader_translate(&es, &key);
    assert(shader);
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(shader, 1, strlen(shader), file) == strlen(shader));
    assert(fclose(file) == 0);
    free(shader);
}

int main(int argc, char **argv)
{
    struct nv2a_dialect gl = {.version = 450};
    struct nv2a_dialect metal = {.version = GPU_SHADER_LANGUAGE_MSL, .msl = 1};
    struct nv2a_dialect es = {.version = 310, .es = 1};

    assert(argc == 3);
    check(&gl, 0, 0);
    check(&gl, 1, 0);
    check(&metal, 0, 0);
    check(&metal, 1, 0);
    check(&es, 1, 0);
    check(&es, 1, 1);
    write_es(argv[1], 0);
    write_es(argv[2], 1);
    return 0;
}
'''


@pytest.mark.skipif(sys.platform != "linux", reason="the production translator uses the 32-bit Linux ABI")
def test_dot_zw_depth_generation_for_gl_and_metal():
    if shutil.which("glslangValidator") is None:
        pytest.skip("install glslang to compile the generated ES shaders")
    objects = [ROOT / "build/linux/obj/port/linux/src" / name for name in
               ("nv2a_psh.o", "nv2a_msl.o", "nv2a_vsh.o", "xgpu_text.o")]
    assert all(path.exists() for path in objects), "build the Linux release before this test"
    with tempfile.TemporaryDirectory(prefix="zsprite-depth-") as directory:
        program = Path(directory, "probe.c")
        binary = Path(directory, "probe")
        normal = Path(directory, "dot_zw.frag")
        counter = Path(directory, "dot_zw_counter.frag")
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
        subprocess.run([str(binary), str(normal), str(counter)], cwd=ROOT, check=True)
        for shader in (normal, counter):
            subprocess.run(["glslangValidator", str(shader)], cwd=ROOT, check=True)
