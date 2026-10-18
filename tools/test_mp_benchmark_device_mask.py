#!/usr/bin/env python3
"""Test the XInput device report using its production function bodies."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "port/linux/src/xinput_sdl.c").read_text()


def function_source(signature: str) -> str:
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth = 0
    for position in range(opening, len(SOURCE)):
        if SOURCE[position] == "{":
            depth += 1
        elif SOURCE[position] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:position + 1]
    raise ValueError(f"Unclosed function: {signature}")


HARNESS = r'''
#include <stdio.h>
#include <string.h>
#define PORT_COUNT 4
#define WINAPI
#define FALSE 0
#define TRUE 1
typedef int BOOL;
typedef unsigned long DWORD;
typedef DWORD *PDWORD;
typedef void *PXPP_DEVICE_TYPE;
typedef int SDL_Gamepad;
static int gamepad_type, keyboard_type;
#define XDEVICE_TYPE_GAMEPAD ((PXPP_DEVICE_TYPE)&gamepad_type)
#define XDEVICE_TYPE_DEBUG_KEYBOARD ((PXPP_DEVICE_TYPE)&keyboard_type)
#define XDEVICE_PORT0_MASK 1UL
static DWORD reported_gamepads;
static BOOL reported_keyboard;
static int probe_requested, physical_count;
static SDL_Gamepad pads[PORT_COUNT];
int mp_benchmark_probe_requested(void) { return probe_requested; }
unsigned long mp_benchmark_probe_device_mask(unsigned long connected, int requested);
static int sdl_gamepads(SDL_Gamepad *gamepads[PORT_COUNT])
{
    for (int port = 0; port < PORT_COUNT; port++)
        gamepads[port] = port < physical_count ? &pads[port] : NULL;
    return physical_count;
}
static SDL_Gamepad *port_gamepad(SDL_Gamepad *gamepads[PORT_COUNT], int count, int port)
{
    return port < count ? gamepads[port] : NULL;
}
__CONNECTION_FUNCTIONS__
static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "device mask: line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
int main(void)
{
    DWORD insertions, removals;
    CHECK(XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &insertions, &removals));
    CHECK(insertions == 1 && removals == 0);
    probe_requested = 1;
    CHECK(XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &insertions, &removals));
    CHECK(insertions == 14 && removals == 0);
    physical_count = 2;
    CHECK(!XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &insertions, &removals));
    CHECK(insertions == 0 && removals == 0);
    probe_requested = 0;
    CHECK(XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &insertions, &removals));
    CHECK(insertions == 0 && removals == 12);
    physical_count = 0;
    CHECK(XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &insertions, &removals));
    CHECK(insertions == 0 && removals == 2);
    return failures ? 1 : 0;
}
'''


def test_virtual_device_report() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise SkipTest("A C compiler is required for the device-mask probe")
    functions = "\n\n".join((
        function_source("static DWORD connected_gamepads(void)"),
        function_source("BOOL WINAPI XGetDeviceChanges("),
    ))
    with tempfile.TemporaryDirectory(prefix="mp-device-mask-") as temporary:
        source = Path(temporary) / "device_mask.c"
        executable = Path(temporary) / "device_mask"
        source.write_text(HARNESS.replace("__CONNECTION_FUNCTIONS__", functions))
        subprocess.run([
            compiler, "-O2", "-Wall", "-Wextra", "-Werror", "-iquote",
            str(ROOT / "port/linux/include"), str(source),
            str(ROOT / "port/linux/src/mp_benchmark_probe.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    test_virtual_device_report()
