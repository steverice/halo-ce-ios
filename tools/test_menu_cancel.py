"""Run the real widget event dispatcher against the shipped menu handlers.

Removing the root's inert cancel handlers must reproduce the empty UI-map
screen. The tag loader and game boundaries are replaced, not cancel dispatch,
history navigation, or handler dispatch. No game data or GPU is required.
"""
import json
from pathlib import Path
import re
import runpy
import subprocess
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_cancel_preserves_the_main_menu_and_keeps_back_navigation(tmp_path):
    source = (ROOT / "source/interface/ui_widget.c").read_text()
    enums = "\n".join(match.group() for match in re.finditer(r"enum\s*\{[^}]*\};", source)
                      if any(name in match.group() for name in (
                          "_widget_pass_unhandled_events_to_children_bit",
                          "_event_handler_close_current_widget_bit",
                          "_conditional_widget_load_if_event_handler_function_fails_bit",
                          "_event_type_null", "_widget_event_b_button", "DPAD_EVENT_REPEAT_MILLISECONDS")))
    functions = "\n".join(block(source, signature) for signature in (
        "static void widget_instance_go_back_to_previous(\n\tstruct widget_instance *widget)\n{",
        "static void event_handler_dispatch(\n\tstruct widget_instance *widget,\n\tstruct ui_widget_definition *definition,\n\tstruct event_record *event,\n\tstruct ui_widget_event_handler_reference *handler,\n\tboolean *calling_widget_deleted)\n{",
        "static void widget_instance_process_one_event_recursive(\n\tstruct widget_instance *widget,\n\tstruct ui_widget_definition *definition,\n\tstruct event_record *event,\n\tboolean *return_widget_deleted)\n{"))
    main_menu = ElementTree.parse(ROOT / "port/assets/menus/ce/main_menu.xml").getroot()
    root = main_menu.find("widget[@name='main_menu/main_menu']")
    # ce_menus.widget_xml writes these patch handlers verbatim. Exercise both
    # the shipped XML and the regenerated handlers, so regeneration cannot
    # silently restore the root dismissal.
    patches = runpy.run_path(str(ROOT / "tools/port_settings.py"))["WIDGET_PATCHES"]
    generated = ElementTree.fromstring("<widget>" + "".join(patches["main_menu/main_menu"]["handlers"]) + "</widget>")
    handlers = []
    for menu_source, menu in enumerate((root, generated)):
        handlers.append(f"if (menu_source == {menu_source}) {{")
        for on in menu.findall("on"):
            if on.get("event") not in ("b", "back"):
                continue
            # Keep the runtime's platform decision in C, including HALO_APP_CAN_QUIT.
            event = "_widget_event_b_button" if on.get("event") == "b" else "_widget_event_back_button"
            platform = on.get("platform")
            condition = f"menu_platform_matches({json.dumps(platform)})" if platform else "1"
            flags = "1 << _event_handler_open_widget_bit" if on.get("open") else "0"
            assert set(on.attrib) <= {"event", "platform", "open"}, on.attrib
            handlers.append(f"if ({condition}) add_handler({event}, {flags});")
        handlers.append("}")
    fixture = (ROOT / "port/ios/tests/menu_cancel_probe.c").read_text()
    program = fixture.replace("/* ENUMS */", enums).replace("/* PRODUCTION */", functions)
    keyboard = (ROOT / "port/linux/src/xinput_sdl.c").read_text()
    program = program.replace("/* KEYBOARD */", "\n".join(block(keyboard, signature) for signature in (
        "static BYTE analog(BOOL down)",
        "static void keyboard_gamepad(const struct platform_input_state *input, XINPUT_GAMEPAD *pad)")))
    program = program.replace("/* ROOT HANDLERS */", "\n".join(handlers))
    probe, binary = tmp_path / "menu_cancel.c", tmp_path / "menu_cancel"
    probe.write_text(program)
    sdl_flags = subprocess.check_output(["pkg-config", "--cflags", "sdl3"], text=True).split()
    subprocess.run(["xcrun", "clang", "-Wall", "-Werror", "-Wno-unused-variable", *sdl_flags,
                    "-fsanitize=address,undefined", "--include-directory=" + str(ROOT / "port/linux/src"),
                    "-DHALO_ILP32=1", str(probe), "--output=" + str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    import tempfile

    with tempfile.TemporaryDirectory() as directory:
        test_cancel_preserves_the_main_menu_and_keeps_back_navigation(Path(directory))
