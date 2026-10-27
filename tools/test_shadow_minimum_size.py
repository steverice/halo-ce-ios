"""Exercise display.shadow_minimum_size's floor, fade and exemptions in the real C header."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

PROGRAM = r'''
#include <assert.h>
#include "source/render/shadow_minimum_size.h"
int main(void) {
    long roots[2] = {7, 12};
    float x;
    /* off, or at or below the game's floor: the game's 30 and its own fade, to the bit */
    assert(shadow_minimum_size_pixels(0, 0) == 30.f);
    assert(shadow_minimum_size_pixels(30, 0) == 30.f);
    assert(shadow_minimum_size_pixels(12, 0) == 30.f);
    for (x = 0.f; x < 400.f; x += 0.37f) {
        assert(!shadow_minimum_size_culled(x, 30.f));
        assert(shadow_minimum_size_fraction(x, 30.f) == (x - 30.f) * 0.06666667f);
    }
    /* a raised floor: culled at or below it, invisible at it, full at one and a half times it */
    assert(shadow_minimum_size_pixels(90, 0) == 90.f);
    assert(shadow_minimum_size_culled(60.f, 90.f) && shadow_minimum_size_culled(90.f, 90.f));
    assert(!shadow_minimum_size_culled(90.5f, 90.f));
    assert(shadow_minimum_size_fraction(90.f, 90.f) == 0.f);
    assert(shadow_minimum_size_fraction(135.f, 90.f) == 1.f);
    assert(shadow_minimum_size_fraction(100.f, 90.f) > 0.f && shadow_minimum_size_fraction(100.f, 90.f) < 0.25f);
    /* the fade only grows as the object grows: no step where it starts */
    for (x = 90.f; x < 200.f; x += 0.5f)
        assert(shadow_minimum_size_fraction(x + 0.5f, 90.f) > shadow_minimum_size_fraction(x, 90.f));
    /* the local players' units, vehicles and fellow riders keep the game's floor */
    assert(shadow_minimum_size_exempt(12, roots, 2) && shadow_minimum_size_exempt(7, roots, 2));
    assert(!shadow_minimum_size_exempt(8, roots, 2) && !shadow_minimum_size_exempt(7, roots, 0));
    assert(shadow_minimum_size_pixels(90, 1) == 30.f);
    return 0;
}
'''


def build_and_run(tmp_path, flags):
    source = tmp_path / 'shadow_minimum_size.c'
    binary = tmp_path / ('shadow_minimum_size.exe' if os.name == 'nt' else 'shadow_minimum_size')
    source.write_text(PROGRAM)
    subprocess.run([shutil.which('clang') or 'cc', *flags, '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_floor_fade_and_exemptions(tmp_path):
    build_and_run(tmp_path, [])


def test_header_is_c89(tmp_path):
    build_and_run(tmp_path, ['-std=c89', '-pedantic-errors', '-D__STRICT_ANSI__'])


def test_render_objects_reads_the_setting_once_a_frame():
    text = (ROOT / 'source/render/render_objects.c').read_text()
    assert text.count('config_integer("display.shadow_minimum_size")') == 1
    begin = text.index('void render_object_shadows(')
    assert 'shadow_minimum_size_begin();' in text[begin:text.index('profile_exit(render_object_shadows_section)', begin)]
