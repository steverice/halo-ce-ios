"""Exercise cap membership, protected weapon lights, and deterministic ranking in the real C selector."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_selection(tmp_path):
    program = r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include "source/objects/light_render_selection.h"
int main(void) {
    struct light_render_candidate lights[] = {
        {9, 100, 0, 1}, {2, 100, 0, 1}, {3, 200, 0, 0}, {4, 0, 1, 0}, {5, 0, 1, 1}
    };
    struct light_render_candidate saved[5];
    unsigned char d[5], s[5];
    memcpy(saved, lights, sizeof(lights));
    light_render_select(lights, 5, 0, 0, d, s);
    for (int i = 0; i < 5; ++i) { assert(d[i]); assert(s[i] == lights[i].specular); }
    light_render_select(lights, 5, 4, 2, d, s);
    assert(!d[0] && d[1] && d[2] && d[3] && d[4]);
    assert(!s[0] && s[1] && !s[2] && !s[3] && s[4]);
    light_render_select(lights, 5, 1, 1, d, s);
    assert(!d[0] && !d[1] && !d[2] && d[3] && d[4]);
    assert(!s[0] && !s[1] && !s[2] && !s[3] && s[4]);
    light_render_select(lights, 5, 0, 1, d, s);
    assert(d[0] && d[1] && d[2] && d[3] && d[4]);
    assert(!s[0] && !s[1] && !s[2] && !s[3] && s[4]);
    light_render_select(lights, 5, 3, 0, d, s);
    assert(!d[0] && !d[1] && d[2] && d[3] && d[4]);
    assert(!s[0] && !s[1] && !s[2] && !s[3] && s[4]);
    assert(!memcmp(saved, lights, sizeof(lights)));
    /* Changing enumeration order cannot change selected identifiers. */
    for (int i = 0; i < 5; ++i) lights[i] = saved[4-i];
    light_render_select(lights, 5, 4, 2, d, s);
    for (int i = 0; i < 5; ++i) {
        assert(d[i] == (lights[i].id != 9));
        assert(s[i] == (lights[i].id == 2 || lights[i].id == 5));
    }
    assert(light_render_score(2, 4, 0, 0, 0, 0, 0) == light_render_score(1, 2, 0, 0, 0, 0, 0));
    assert(isfinite(light_render_score(2, 0, 0, 0, 0, 0, 0)));
    assert(light_render_score(0, 0, 0, 0, 0, 0, 0) == 0);
    assert(light_render_score(NAN, 0, 0, 0, 0, 0, 0) == 0);
    assert(light_render_score(2, INFINITY, 0, 0, 0, 0, 0) == 0);
    light_render_select(lights, 0, 1, 1, d, s);
    return 0;
}
'''
    source = tmp_path / 'selection.c'
    binary = tmp_path / ('selection.exe' if os.name == 'nt' else 'selection')
    source.write_text(program)
    subprocess.run([shutil.which('clang') or 'cc', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_selector_uses_c89_headers(tmp_path):
    source = tmp_path / 'c89.c'
    binary = tmp_path / ('c89.exe' if os.name == 'nt' else 'c89')
    source.write_text(r'''
#include "source/objects/light_render_selection.h"
int main(void) {
    struct light_render_candidate light;
    unsigned char diffuse, specular;
    light.id = 1; light.weapon = 0; light.specular = 1;
    light.score = light_render_score(1, 2, 0, 0, 0, 0, 0);
    light_render_select(&light, 1, 1, 1, &diffuse, &specular);
    return !(diffuse && specular && light.score == 0.25);
}
''')
    subprocess.run([shutil.which('clang') or 'cc', '-std=c89', '-pedantic-errors', '-D__STRICT_ANSI__',
                    '-Wall', '-Wextra', '-Werror', '-I', str(ROOT), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
