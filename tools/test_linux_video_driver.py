"""Exercise the production NVIDIA/Wayland selection code with simulated drivers."""

from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "port/linux/src/sdl_platform.c").read_text()


def test_nvidia_wayland_driver_selection(tmp_path: Path) -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        pytest.skip("a C compiler is required for the video driver probe")

    marker = "static void platform_choose_video_driver(void)"
    assert SOURCE.count(marker) == 1
    function = marker + SOURCE.split(marker, 1)[1].split("\n#endif", 1)[0]
    assert SOURCE.index("platform_choose_video_driver();") < SOURCE.index("SDL_Init(SDL_INIT_VIDEO")

    harness = r'''
#include <assert.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int explicit_driver, wayland, nvidia, first_library, second_library;
static int opens, closes, hints, logs;
static const char *last_hint;
static const char *fake_getenv(const char *name)
{
    if (strcmp(name, "SDL_VIDEO_DRIVER") == 0)
        return explicit_driver ? "wayland" : NULL;
    assert(strcmp(name, "WAYLAND_DISPLAY") == 0);
    return wayland ? "wayland-0" : NULL;
}
static int fake_access(const char *path, int mode)
{
    assert(strcmp(path, "/sys/module/nvidia") == 0 && mode == F_OK);
    return nvidia ? 0 : -1;
}
static void *fake_dlopen(const char *name, int mode)
{
    assert(mode == (RTLD_LAZY | RTLD_LOCAL));
    opens++;
    if (strcmp(name, "libnvidia-egl-wayland.so.1") == 0)
        return first_library ? &opens : NULL;
    assert(strcmp(name, "libnvidia-egl-wayland2.so.1") == 0);
    return second_library ? &opens : NULL;
}
static int fake_dlclose(void *handle)
{
    assert(handle == &opens);
    closes++;
    return 0;
}
static int fake_set_hint(const char *name, const char *value)
{
    assert(strcmp(name, "SDL_VIDEO_DRIVER") == 0);
    hints++;
    last_hint = value;
    return 1;
}
static void fake_log(const char *format, ...)
{
    assert(strstr(format, "NVIDIA") != NULL);
    logs++;
}
#define getenv fake_getenv
#define access fake_access
#define dlopen fake_dlopen
#define dlclose fake_dlclose
#define SDL_SetHint fake_set_hint
#define platform_log fake_log
#define SDL_HINT_VIDEO_DRIVER "SDL_VIDEO_DRIVER"
__FUNCTION__

static void reset(void)
{
    explicit_driver = wayland = nvidia = first_library = second_library = 0;
    opens = closes = hints = logs = 0;
    last_hint = NULL;
}
int main(void)
{
    reset();
    platform_choose_video_driver();
    assert(opens == 0 && hints == 0);

    reset(); wayland = nvidia = explicit_driver = 1;
    platform_choose_video_driver();
    assert(opens == 0 && hints == 0);

    reset(); nvidia = 1;
    platform_choose_video_driver();
    assert(opens == 0 && hints == 0);

    reset(); wayland = 1;
    platform_choose_video_driver();
    assert(opens == 0 && hints == 0);

    reset(); wayland = nvidia = first_library = 1;
    platform_choose_video_driver();
    assert(opens == 1 && closes == 1 && hints == 0);

    reset(); wayland = nvidia = second_library = 1;
    platform_choose_video_driver();
    assert(opens == 2 && closes == 1 && hints == 0);

    reset(); wayland = nvidia = 1;
    platform_choose_video_driver();
    assert(opens == 2 && closes == 0 && hints == 1 && logs == 1);
    assert(strcmp(last_hint, "x11,wayland") == 0);
    return 0;
}
'''.replace("__FUNCTION__", function)
    output = tmp_path / "video-driver-probe"
    build = subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o", str(output)],
        input=harness,
        text=True,
        capture_output=True,
        check=False,
    )
    assert build.returncode == 0, build.stderr
    result = subprocess.run([str(output)], text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stderr
