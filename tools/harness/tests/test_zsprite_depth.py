"""Check the actual pixel-shader translator's DOT_ZW output for GL and Metal."""

import hashlib
import json
import os
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

static struct nv2a_pixel_shader_key make_key(int dot_zw, int count_samples)
{
    struct nv2a_pixel_shader_key key = {0};

    key.texture_modes = 0x04UL | (0x11UL << (5 * 2));
    if (dot_zw)
        key.texture_modes |= 0x0aUL << (5 * 3);
    key.combiner_state[D3DRS_PSFINALCOMBINERINPUTSABCD] = 0x04;
    key.combiner_state[D3DRS_PSFINALCOMBINERINPUTSEFG] = 0x14 << 8;
    key.count_samples = count_samples;
    return key;
}

static void check(const struct nv2a_dialect *dialect, int dot_zw, int count_samples)
{
    struct nv2a_pixel_shader_key key = make_key(dot_zw, count_samples);
    char *shader;

    shader = nv2a_pixel_shader_translate(dialect, &key);
    assert(shader);
    if (dot_zw && !(dialect->es && count_samples))
    {
        assert(strstr(shader, "replaced_depth = dot3 > 0.0 ? clamp(dot2 / dot3 / 16777215.0") != NULL);
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

static void write_shader(const char *directory, const char *name, const char *extension,
	const struct nv2a_dialect *dialect, int dot_zw, int count_samples, int discard)
{
    struct nv2a_pixel_shader_key key = make_key(dot_zw, count_samples);
    char path[1024];
    FILE *file;
    char *shader;

    if (discard)
        key.alpha_test_function = D3DCMP_NEVER;
    shader = nv2a_pixel_shader_translate(dialect, &key);
    assert(shader);
    assert(snprintf(path, sizeof(path), "%s/%s.%s", directory, name, extension) < (int)sizeof(path));
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(shader, 1, strlen(shader), file) == strlen(shader));
    assert(fclose(file) == 0);
    assert(snprintf(path, sizeof(path), "%s/%s.key", directory, name) < (int)sizeof(path));
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(&key, 1, sizeof(key), file) == sizeof(key));
    assert(fclose(file) == 0);
    free(shader);
}

int main(int argc, char **argv)
{
    struct nv2a_dialect gl = {.version = 410};
    struct nv2a_dialect metal = {.version = GPU_SHADER_LANGUAGE_MSL, .msl = 1};
    struct nv2a_dialect es = {.version = 310, .es = 1};
    struct nv2a_dialect gl_debug = gl;
    struct nv2a_dialect metal_debug = metal;

    assert(argc == 2);
    gl_debug.debug_expression = "vec3(0.0, 1.0, 0.0)";
    metal_debug.debug_expression = gl_debug.debug_expression;
    check(&gl, 0, 0);
    check(&gl, 1, 0);
    check(&metal, 0, 0);
    check(&metal, 1, 0);
    check(&es, 1, 0);
    check(&es, 1, 1);
    write_shader(argv[1], "gl_dot", "frag", &gl, 1, 0, 0);
    write_shader(argv[1], "gl_control", "frag", &gl, 0, 0, 0);
    write_shader(argv[1], "gl_dot_debug", "frag", &gl_debug, 1, 0, 0);
    write_shader(argv[1], "gl_dot_discard", "frag", &gl, 1, 0, 1);
    write_shader(argv[1], "msl_dot", "metal", &metal, 1, 0, 0);
    write_shader(argv[1], "msl_control", "metal", &metal, 0, 0, 0);
    write_shader(argv[1], "msl_dot_debug", "metal", &metal_debug, 1, 0, 0);
    write_shader(argv[1], "msl_dot_discard", "metal", &metal, 1, 0, 1);
    write_shader(argv[1], "es_dot", "frag", &es, 1, 0, 0);
    write_shader(argv[1], "es_counter", "frag", &es, 1, 1, 0);
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
        program.write_text(PROBE)
        subprocess.run([
            "clang", "--target=i686-linux-gnu", "-m32", "-no-pie", "-flto=full", "-fuse-ld=lld",
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
        subprocess.run([str(binary), directory], cwd=ROOT, check=True)
        for shader in (Path(directory, "es_dot.frag"), Path(directory, "es_counter.frag")):
            subprocess.run(["glslangValidator", str(shader)], cwd=ROOT, check=True)
        output = os.environ.get("ZSPRITE_SHADER_OUTPUT")
        if output:
            destination = ROOT / output
            destination.mkdir(parents=True, exist_ok=True)
            hashes = {}
            for path in sorted(Path(directory).glob("*")):
                if path.suffix not in (".frag", ".metal", ".key"):
                    continue
                contents = path.read_bytes()
                (destination / path.name).write_bytes(contents)
                hashes[path.name] = {"sha256": hashlib.sha256(contents).hexdigest(), "bytes": len(contents)}
            head = os.environ.get("GITHUB_SHA") or subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
            (destination / "manifest.json").write_text(json.dumps({"head": head, "files": hashes}, indent=2) + "\n")
