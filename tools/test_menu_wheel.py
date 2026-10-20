"""Run the menu's real wheel selector against a focused button bar."""

from pathlib import Path
import shutil
import subprocess
import sys
from unittest import SkipTest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from harness import function  # noqa: E402


SOURCE = ROOT / "source/interface/ui_widget.c"


def test_wheel_leaves_horizontal_button_bar_for_vertical_list(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the menu wheel probe")

    source = SOURCE.read_text()
    functions = "\n".join(function(source, name) for name in (
        "ui_mouse_list_directions", "ui_mouse_wheel_widget"))
    (tmp_path / "under_test.inc").write_text(functions)
    program = tmp_path / "wheel.c"
    program.write_text(r'''
#include <assert.h>
#include <stddef.h>
#define FLAG(bit) (1UL << (bit))
#define TEST_FLAG(flags, bit) (((flags) & FLAG(bit)) != 0)
#define _widget_dpad_updown_tabs_thru_children_bit 0
#define _widget_dpad_leftright_tabs_thru_children_bit 1
#define _widget_dpad_updown_tabs_thru_list_items_bit 2
#define _widget_dpad_leftright_tabs_thru_list_items_bit 3
#define _widget_event_dpad_up 10
#define _widget_event_dpad_down 11
#define _widget_event_dpad_left 12
#define _widget_event_dpad_right 13
struct ui_widget_definition { unsigned long flags; };
struct widget_instance { long definition_tag_index; struct widget_instance *focused_child; };
static struct ui_widget_definition definitions[3];
static struct ui_widget_definition *ui_widget_definition_get(long index) { return &definitions[index]; }
#include "under_test.inc"
int main(void) {
    struct widget_instance bar = {2, NULL};
    struct widget_instance list = {1, &bar};
    struct widget_instance root = {0, &list};
    struct widget_instance *selected;
    short back, forward;

    definitions[0].flags = FLAG(_widget_dpad_updown_tabs_thru_children_bit);
    definitions[1].flags = FLAG(_widget_dpad_updown_tabs_thru_list_items_bit);
    definitions[2].flags = FLAG(_widget_dpad_leftright_tabs_thru_children_bit);
    selected = ui_mouse_wheel_widget(&root);
    assert(selected == &list);
    ui_mouse_list_directions(selected, &back, &forward);
    assert(back == _widget_event_dpad_up && forward == _widget_event_dpad_down);

    selected = ui_mouse_wheel_widget(&bar);
    assert(selected == &bar);
    ui_mouse_list_directions(selected, &back, &forward);
    assert(back == _widget_event_dpad_left && forward == _widget_event_dpad_right);
    return 0;
}
''')
    binary = tmp_path / "wheel"
    subprocess.run([compiler, "-std=gnu99", "-Wall", "-Werror", "-I", str(tmp_path),
                    str(program), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
