"""Execute the production movie and shader policies with controlled config values."""
import shutil
import subprocess
from pathlib import Path

import pytest

from test_death_timing import function

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu.h"
typedef int BOOL;
#define TRUE 1
#define FALSE 0
static const char *mode, *key, *value;
static const char *config_string(const char *name)
{
    if (!strcmp(name, "display.movies") || !strcmp(name, "display.async_shaders"))
        return mode;
    assert(!strcmp(name, "debug.input_replay") || !strcmp(name, "debug.benchmark_suite"));
    return !strcmp(name, key) ? value : "";
}
static int config_boolean(const char *name)
{
    assert(!strcmp(name, "debug.benchmark"));
    return !strcmp(name, key) ? atoi(value) : 0;
}
static double config_real(const char *name)
{
    assert(!strcmp(name, "debug.exit_after"));
    return !strcmp(name, key) ? atof(value) : 0.0;
}
static int config_integer(const char *name)
{
    assert(!strcmp(name, "debug.screenshot_every"));
    return !strcmp(name, key) ? atoi(value) : 0;
}
static int platform_fixed_timestep(void)
{
    return !strcmp(key, "debug.fixed_timestep") ? atoi(value) : 0;
}
static void platform_log(const char *format, ...) { (void)format; }
'''


@pytest.fixture(scope="session")
def policy_probe(tmp_path_factory):
    compiler = shutil.which("clang")
    if not compiler:
        pytest.skip("clang is needed for the native policy probe")
    build = tmp_path_factory.mktemp("deterministic-policies")
    source = build / "probe.c"
    # Compile the actual functions, as in test_death_timing; no policy logic
    # is copied into the test. Config and the fixed-timestep clock are inputs.
    source.write_text(PRELUDE + "\n" + function(
        (ROOT / "port/runtime/guest/runtime/guest_bink.c").read_text(), "movies_enabled"
    ) + "\n" + function(
        (ROOT / "port/linux/src/d3d8_device.c").read_text(), "async_shader_flags"
    ) + r'''
int main(int argc, char **argv)
{
    assert(argc == 4);
    mode = argv[1]; key = argv[2]; value = argv[3];
    printf("%d %d\n", movies_enabled(),
        (async_shader_flags() & GPU_INITIALIZE_ASYNC_SHADERS) != 0);
    return 0;
}
''')
    binary = build / "probe"
    subprocess.run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "port/linux/src"), str(source), "-o", str(binary)], check=True)
    return binary


def policies(probe, mode, key, value):
    result = subprocess.run([str(probe), mode, key, value], check=True, capture_output=True, text=True)
    return tuple(map(int, result.stdout.split()))


@pytest.mark.parametrize("suite", ["quick", "full", "quick:a10-crew,c40-bridge"])
def test_native_suite_disables_automatic_movies_and_async_shaders(policy_probe, suite):
    assert policies(policy_probe, "auto", "debug.benchmark_suite", suite) == (0, 0)


def test_empty_suite_keeps_normal_automatic_movies_and_async_shaders(policy_probe):
    assert policies(policy_probe, "auto", "debug.benchmark_suite", "") == (1, 1)


@pytest.mark.parametrize("key,value", [
    ("debug.fixed_timestep", "1"),
    ("debug.input_replay", "route"),
    ("debug.benchmark", "1"),
    ("debug.exit_after", "1.5"),
    ("debug.screenshot_every", "2"),
])
def test_existing_deterministic_triggers_still_disable_automatic_policies(policy_probe, key, value):
    assert policies(policy_probe, "auto", key, value) == (0, 0)


@pytest.mark.parametrize("suite", ["", "quick", "full"])
@pytest.mark.parametrize("mode,expected", [("on", (1, 1)), ("off", (0, 0))])
def test_explicit_modes_override_the_native_suite(policy_probe, suite, mode, expected):
    assert policies(policy_probe, mode, "debug.benchmark_suite", suite) == expected


@pytest.mark.parametrize("suite,expected", [("", (1, 1)), ("quick", (0, 0))])
def test_unknown_mode_uses_automatic_policy(policy_probe, suite, expected):
    assert policies(policy_probe, "unknown", "debug.benchmark_suite", suite) == expected
