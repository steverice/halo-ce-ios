"""Execute the production touch reader with held axes and pause-menu changes."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]


def test_paused_touch_sticks_and_shared_hardware():
    source = (ROOT / "port/ios/host/host_touch.m").read_text()
    start = source.index("int host_ios_gamepad_axis(")
    end = source.index("\nint host_ios_gamepad_button(", start)
    reader = source[start:end]
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the touch pause probe")
    harness = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "host_touch_replay.h"
typedef struct { int id; int axes[6]; } SDL_Gamepad;
enum { SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
       SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER };
static struct host_touch_replay_gate replay_gate;
static int touch_id = 1, primary_hardware, paused;
static SDL_Gamepad touch = {1, {12000, -18000, 14000, -20000, 9000, 15000}};
static SDL_Gamepad hardware = {2, {-24000, 26000, -28000, 30000, 18000, 21000}};
static int SDL_GetGamepadID(SDL_Gamepad *pad) { return pad->id; }
static int SDL_GetGamepadAxis(SDL_Gamepad *pad, int axis) { return pad->axes[axis]; }
static SDL_Gamepad *SDL_GetGamepadFromID(int id) { assert(id == 2); return &hardware; }
static int __attribute__((unused)) SDL_GetHintBoolean(const char *name, int fallback) {
    assert(!strcmp(name, "VISR_PAUSE_MENU")); (void)fallback; return paused;
}
__READER__
int main(void) {
    for (int axis = 0; axis < 6; axis++)
        assert(host_ios_gamepad_axis(&touch, axis) == touch.axes[axis]);
    paused = 1;
    /* A finger already held when pause opens must stop immediately. */
    for (int axis = 0; axis < 4; axis++)
        assert(host_ios_gamepad_axis(&touch, axis) == 0);
    /* Continuing or beginning a drag while paused cannot feed movement/look. */
    for (int axis = 0; axis < 4; axis++) {
        touch.axes[axis] = axis % 2 ? -32767 : 32767;
        assert(host_ios_gamepad_axis(&touch, axis) == 0);
    }
    /* Triggers and hardware retain their existing behavior. */
    assert(host_ios_gamepad_axis(&touch, 4) == 9000);
    assert(host_ios_gamepad_axis(&touch, 5) == 15000);
    primary_hardware = 2;
    for (int axis = 0; axis < 6; axis++) {
        assert(host_ios_gamepad_axis(&touch, axis) == hardware.axes[axis]);
        assert(host_ios_gamepad_axis(&hardware, axis) == hardware.axes[axis]);
    }
    /* An idle shared controller must not let held touch axes leak through. */
    for (int axis = 0; axis < 4; axis++) {
        hardware.axes[axis] = 0;
        assert(host_ios_gamepad_axis(&touch, axis) == 0);
    }
    paused = 0; primary_hardware = 0;
    for (int axis = 0; axis < 6; axis++)
        assert(host_ios_gamepad_axis(&touch, axis) == touch.axes[axis]);
    atomic_store(&replay_gate.active, 1);
    for (int axis = 0; axis < 6; axis++)
        assert(host_ios_gamepad_axis(&touch, axis) == 0);
    primary_hardware = 2; paused = 1;
    for (int axis = 0; axis < 6; axis++)
        assert(host_ios_gamepad_axis(&touch, axis) == hardware.axes[axis]);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="touch-pause-") as temporary:
        probe = Path(temporary) / "touch_pause.c"
        executable = Path(temporary) / "touch_pause"
        probe.write_text(harness.replace("__READER__", reader))
        subprocess.run([compiler, "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "port/ios/host"),
                        str(probe), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    test_paused_touch_sticks_and_shared_hardware()
