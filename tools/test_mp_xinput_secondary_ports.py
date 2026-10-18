#!/usr/bin/env python3
"""Exercise the production XInputGetState branch for secondary controller ports."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "port/linux/src/xinput_sdl.c").read_text()


def secondary_branch() -> str:
    start = SOURCE.index("DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)")
    end = SOURCE.index("DWORD WINAPI XInputSetState(", start)
    function = SOURCE[start:end]
    branch = function.index('else if (port < config_integer("debug.network_test_players")')
    finish = function.index("\n\tif (memcmp(&state->Gamepad", branch)
    return function[branch:finish]


HARNESS = r'''
#include <stdio.h>
#include <string.h>
#define PORT_COUNT 4
typedef struct { int value; } XINPUT_GAMEPAD;
typedef struct { XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef XINPUT_STATE *PXINPUT_STATE;
typedef int SDL_Gamepad;
static int requested, configured_players = 1, physical_count, virtual_calls, physical_calls;
static SDL_Gamepad pads[PORT_COUNT];
static long config_integer(const char *key) { (void)key; return configured_players; }
static int __attribute__((unused)) mp_benchmark_probe_requested(void) { return requested; }
static int mp_benchmark_probe_or_test_gamepad(XINPUT_GAMEPAD *pad, short port) {
    virtual_calls++; pad->value = 100 + port; return 1;
}
static SDL_Gamepad *port_gamepad(SDL_Gamepad *gamepads[PORT_COUNT], int count, int port) {
    return port < count ? gamepads[port] : NULL;
}
static void sdl_gamepad_state(SDL_Gamepad *pad, XINPUT_GAMEPAD *state) {
    physical_calls++; state->value = *pad;
}
static void actual_xinput_secondary_branch(int port, PXINPUT_STATE state) {
    SDL_Gamepad *gamepads[PORT_COUNT] = { 0 };
    int count = physical_count;
    for (int index = 0; index < count; index++) gamepads[index] = &pads[index];
    if (port == 0) { }
    __SECONDARY_BRANCH__
}
static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "secondary XInput line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
int main(void) {
    XINPUT_STATE state = { 0 };
    requested = 1; configured_players = 1; physical_count = 0;
    for (int port = 1; port < PORT_COUNT; port++) {
        state.Gamepad.value = 0;
        actual_xinput_secondary_branch(port, &state);
        CHECK(state.Gamepad.value == 100 + port);
    }
    CHECK(virtual_calls == 3 && physical_calls == 0);
    requested = 0; physical_count = 3; pads[1] = 41; pads[2] = 42;
    state.Gamepad.value = 0;
    actual_xinput_secondary_branch(1, &state);
    CHECK(state.Gamepad.value == 41 && physical_calls == 1 && virtual_calls == 3);
    state.Gamepad.value = 0;
    actual_xinput_secondary_branch(2, &state);
    CHECK(state.Gamepad.value == 42 && physical_calls == 2 && virtual_calls == 3);
    physical_count = 0; configured_players = 2;
    state.Gamepad.value = 0;
    actual_xinput_secondary_branch(1, &state);
    CHECK(state.Gamepad.value == 101 && virtual_calls == 4); /* ordinary scripted test setting */
    return failures != 0;
}
'''


def test_secondary_virtual_ports_and_physical_fallback() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise SkipTest("A C compiler is required for the XInput branch probe")
    with tempfile.TemporaryDirectory(prefix="mp-xinput-ports-") as temporary:
        source = Path(temporary) / "secondary_ports.c"
        executable = Path(temporary) / "secondary_ports"
        source.write_text(HARNESS.replace("__SECONDARY_BRANCH__", secondary_branch()))
        subprocess.run([compiler, "-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    test_secondary_virtual_ports_and_physical_fallback()
