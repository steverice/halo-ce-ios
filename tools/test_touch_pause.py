"""Execute the production touch reader with held axes and pause-menu changes."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]


def test_paused_touch_input_and_shared_hardware():
    source = (ROOT / "port/ios/host/host_touch.m").read_text()
    start = source.find("static int touch_menu_up(")
    if start < 0:
        start = source.index("int host_ios_gamepad_axis(")
    end = source.index("\nvoid host_ios_touch_reset(", start)
    reader = source[start:end]
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the touch pause probe")
    harness = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "host_touch_replay.h"
typedef struct { int id; int axes[6]; unsigned char buttons[16]; } SDL_Gamepad;
enum { SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
       SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START,
       SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
       SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
       SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_COUNT };
static int pause_pending;
enum { SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
       SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER };
static struct host_touch_replay_gate replay_gate;
static int touch_id = 1, primary_hardware, paused;
static SDL_Gamepad touch = {1, {12000, -18000, 14000, -20000, 9000, 15000}, {0}};
static SDL_Gamepad hardware = {2, {-24000, 26000, -28000, 30000, 18000, 21000}, {0}};
static int SDL_GetGamepadButton(SDL_Gamepad *pad, int button) { return pad->buttons[button]; }
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
    /* Fire and grenade (the triggers) are gameplay too. */
    assert(host_ios_gamepad_axis(&touch, 4) == 0);
    assert(host_ios_gamepad_axis(&touch, 5) == 0);
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

    /* Buttons: every touch button down; hardware idle. */
    primary_hardware = 0; atomic_store(&replay_gate.active, 0);
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) touch.buttons[b] = 1;
    paused = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        assert(host_ios_gamepad_button(&touch, b) == 1);
    paused = 1;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        int menu = b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START ||
            b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ||
            b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
        /* Reload/use, weapon switch, crouch, zoom, flashlight and grenade switch must not reach the game. */
        assert(host_ios_gamepad_button(&touch, b) == menu);
    }
    /* Hardware buttons still merge into the shared pad while paused. */
    primary_hardware = 2; touch.buttons[SDL_GAMEPAD_BUTTON_WEST] = 0;
    hardware.buttons[SDL_GAMEPAD_BUTTON_WEST] = 1;
    assert(host_ios_gamepad_button(&touch, SDL_GAMEPAD_BUTTON_WEST) == 1);
    primary_hardware = 0;
    /* The pause button still works from touch while a menu is up. */
    touch.buttons[SDL_GAMEPAD_BUTTON_START] = 0; pause_pending = 1;
    assert(host_ios_gamepad_button(&touch, SDL_GAMEPAD_BUTTON_START) == 1);
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
    test_paused_touch_input_and_shared_hardware()
