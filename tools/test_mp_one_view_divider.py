#!/usr/bin/env python3
"""Exercise the production split-screen divider with visible-window counts."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "source/interface/interface.c"


def divider_source() -> str:
    source = SOURCE.read_text()
    start = source.index("static void interface_splitscreen_render(\n\tvoid)\n{")
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise ValueError("Unclosed split-screen divider function")


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int pixel32;
typedef struct { short x0, y0, x1, y1; } rectangle2d;
static int local_count, visible_count, cinematic, forced;
static int drawn;
static rectangle2d quads[2];
static short halo_screen_width(void) { return 854; }
static short __attribute__((unused)) local_player_count(void) { return (short)local_count; }
static short __attribute__((unused)) main_get_window_count(void) { return (short)visible_count; }
static int game_engine_force_single_screen(void) { return forced; }
static int cinematic_in_progress(void) { return cinematic; }
static void draw_quad(const rectangle2d *bounds, pixel32 color) {
    if (drawn >= 2 || color != 0xff000000u) abort();
    quads[drawn++] = *bounds;
}
#define match_assert(file, line, condition) do { \
    (void)(file); (void)(line); if (!(condition)) abort(); \
} while (0)
__DIVIDER_SOURCE__
static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "divider probe line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
static void render(int locals, int windows, int movie, int single) {
    local_count = locals; visible_count = windows; cinematic = movie; forced = single;
    drawn = 0; memset(quads, 0, sizeof(quads));
    interface_splitscreen_render();
}
int main(void) {
    render(4, 1, 0, 0);
    CHECK(drawn == 0);
    render(4, 4, 0, 0);
    CHECK(drawn == 2);
    CHECK(quads[0].x0 == 0 && quads[0].x1 == 854 && quads[0].y0 == 239 && quads[0].y1 == 241);
    CHECK(quads[1].x0 == 426 && quads[1].x1 == 428 && quads[1].y0 == 0 && quads[1].y1 == 480);
    render(2, 2, 0, 0);
    CHECK(drawn == 1);
    render(3, 3, 0, 0);
    CHECK(drawn == 2 && quads[1].y0 == 240 && quads[1].y1 == 480);
    render(4, 4, 1, 0);
    CHECK(drawn == 0);
    render(4, 4, 0, 1);
    CHECK(drawn == 0);
    render(1, 1, 0, 0);
    CHECK(drawn == 0);
    return failures ? 1 : 0;
}
'''


def test_dividers_follow_visible_windows() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise SkipTest("A C compiler is required for the divider probe")
    with tempfile.TemporaryDirectory(prefix="mp-divider-") as temporary:
        source = Path(temporary) / "divider.c"
        binary = Path(temporary) / "divider"
        source.write_text(HARNESS.replace("__DIVIDER_SOURCE__", divider_source()))
        subprocess.run([compiler, "-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    test_dividers_follow_visible_windows()
