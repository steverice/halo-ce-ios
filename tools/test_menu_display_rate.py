"""Exercise the live Frame Rate label without a game or display."""
from pathlib import Path
import subprocess

from test_menu_cancel import block

ROOT = Path(__file__).resolve().parents[1]


def test_display_rate_label_updates_existing_string_storage(tmp_path):
    source = (ROOT / "port/linux/game/menu_tags.c").read_text()
    refresh = block(source, "int pc_menu_display_rate_refresh(void)")
    program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define HALO_IOS 1
#define NUMBEROF(array) (sizeof(array) / sizeof((array)[0]))
static int rate;
static char display_rate_label[32];
static unsigned short display_rate_unicode[32];
static int host_sdl_display_max_fps(void) { return rate; }
static long halo_menus_utf16(const char *text, unsigned short *out, long size) {
    long i = 0;
    while (text[i] && i < size - 1) { out[i] = (unsigned char)text[i]; i++; }
    out[i] = 0;
    return i;
}
/* REFRESH */
int main(void) {
    unsigned short *existing_label = display_rate_unicode;
    rate = 120;
    assert(pc_menu_display_rate_refresh() == 120);
    assert(!strcmp(display_rate_label, "DISPLAY (120 HZ)"));
    assert(existing_label[9] == '1');
    rate = 60;
    assert(pc_menu_display_rate_refresh() == 60);
    assert(!strcmp(display_rate_label, "DISPLAY (60 HZ)"));
    assert(existing_label[9] == '6' && existing_label[15] == 0);
    rate = 144;
    assert(pc_menu_display_rate_refresh() == 144);
    assert(!strcmp(display_rate_label, "DISPLAY (144 HZ)"));
    rate = 0;
    assert(pc_menu_display_rate_refresh() == 0);
    return 0;
}
'''.replace("/* REFRESH */", refresh)
    probe = tmp_path / "display_rate.c"
    binary = tmp_path / "display_rate"
    probe.write_text(program)
    subprocess.run(["xcrun", "clang", "-Wall", "-Werror", "-fsanitize=address,undefined",
                    str(probe), "--output=" + str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        test_display_rate_label_updates_existing_string_storage(Path(directory))
