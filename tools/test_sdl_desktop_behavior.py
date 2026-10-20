"""Exercise SDL window-mode and mouse capture behavior with an instrumented window."""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from harness import function, mutated  # noqa: E402

SOURCE = (ROOT / "port/linux/src/sdl_platform.c").read_text()
PRELUDE = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define _display_mode_windowed 0
#define _display_mode_fullscreen 1
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed: %s\n", #x); return 1; } } while (0)
typedef struct SDL_Window { int unused; } SDL_Window;
static SDL_Window window, *platform_window = &window;
static BOOL mouse_warp_capture;
static int mouse_warp_x, mouse_warp_y;
static int relative_ok, relative_on, focused, grab_on, cursor_on = 1, warp_x, warp_y, warps;
static int configured_mode, actual_fullscreen, fullscreen_calls, platform_window_mode;
static long platform_window_width = 800, platform_window_height = 600;
static SDL_Window *SDL_GetKeyboardFocus(void) { return focused ? platform_window : NULL; }
static bool SDL_SetWindowRelativeMouseMode(SDL_Window *w, bool on) {
    (void)w;
    if (on && !relative_ok) return false;
    relative_on = on;
    return true;
}
static void SDL_SetWindowMouseGrab(SDL_Window *w, bool on) { (void)w; grab_on = on; }
static void SDL_HideCursor(void) { cursor_on = 0; }
static void SDL_ShowCursor(void) { cursor_on = 1; }
static void SDL_GetWindowSize(SDL_Window *w, int *width, int *height) {
    (void)w; *width = 800; *height = 600;
}
static void SDL_WarpMouseInWindow(SDL_Window *w, float x, float y) {
    (void)w; warp_x = (int)x; warp_y = (int)y; warps++;
}
static int platform_display_mode(void) { return configured_mode; }
static BOOL platform_fullscreen_setting(void) { return configured_mode != _display_mode_windowed; }
static void platform_fullscreen_kind_apply(void) {}
static void platform_window_set_fullscreen(BOOL value) { fullscreen_calls++; actual_fullscreen = value; }
static void platform_window_size_setting(long *width, long *height) { *width = 800; *height = 600; }
static void SDL_SetWindowSize(SDL_Window *w, int width, int height) { (void)w; (void)width; (void)height; }
static int config_boolean(const char *name) { (void)name; return 1; }
static void SDL_GL_SetSwapInterval(int value) { (void)value; }
'''
MAIN = r'''
int main(void) {
    configured_mode = _display_mode_windowed;
    platform_window_mode = _display_mode_windowed;
    actual_fullscreen = 1; /* F11 changed the live window only. */
    platform_display_apply();
    CHECK(actual_fullscreen == 1 && fullscreen_calls == 0);
    configured_mode = _display_mode_fullscreen;
    platform_display_apply();
    CHECK(actual_fullscreen == 1 && fullscreen_calls == 1);
    focused = 1; relative_ok = 0;
    platform_mouse_capture(TRUE);
    CHECK(mouse_warp_capture && grab_on && !cursor_on && warps == 1);
    CHECK(warp_x == 400 && warp_y == 300 && !relative_on);
    platform_mouse_capture(FALSE);
    CHECK(!mouse_warp_capture && !grab_on && cursor_on && !relative_on);
    relative_ok = 1;
    platform_mouse_capture(TRUE);
    CHECK(!mouse_warp_capture && relative_on && !grab_on);
    return 0;
}
'''


def run_probe(tmp_path, *, display_fault=False, mouse_fault=False):
    compiler = shutil.which("clang") or shutil.which("cc")
    assert compiler, "A C compiler is required for the SDL behavior probe"
    tmp_path.mkdir(parents=True, exist_ok=True)
    display = function(SOURCE, "platform_display_apply")
    mouse = function(SOURCE, "platform_mouse_capture")
    if display_fault:
        display = mutated(display, "if (mode != platform_window_mode)", "if (TRUE)")
    if mouse_fault:
        mouse = mutated(mouse, "if (SDL_GetKeyboardFocus() == platform_window &&\n\t\tSDL_SetWindowRelativeMouseMode(platform_window, true))", "if (TRUE)")
    code = PRELUDE + "\n" + mouse + "\n" + display + "\n" + MAIN
    source = tmp_path / "probe.c"
    binary = tmp_path / "probe"
    source.write_text(code)
    compiled = subprocess.run([compiler, "-std=c99", "-Wall", "-Werror", "-Wno-unused-function",
                               str(source), "-o", str(binary)], capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    return subprocess.run([str(binary)], capture_output=True, text=True)


def test_f11_setting_save_and_relative_mouse_fallback(tmp_path):
    observed = run_probe(tmp_path)
    assert observed.returncode == 0, observed.stderr


def test_negative_controls_detect_f11_and_mouse_regressions(tmp_path):
    assert run_probe(tmp_path / "f11", display_fault=True).returncode != 0
    assert run_probe(tmp_path / "mouse", mouse_fault=True).returncode != 0
