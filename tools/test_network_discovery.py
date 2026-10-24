"""Exercise the real LAN compatibility gate against advertised kinds.

The kind bytes are literals from the permitted network-format description.
A missing kind check must let SWAT through and fail this probe.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_advertised_unsupported_kind_is_refused_before_join(tmp_path):
    source = (ROOT / "source/networking/network_client_manager.c").read_text()
    production = function(source, "boolean network_game_client_advertised_game_compatible(\n")
    fixture = (ROOT / "port/ios/tests/network_discovery_probe.c").read_text()
    program = fixture.replace("/* PRODUCTION */", production)
    for name in ("halo_network_version.h", "halo_network_content.h"):
        program = program.replace('"' + name + '"',
                                  '"' + str(ROOT / "port/linux/include" / name) + '"')
    probe, binary = tmp_path / "discovery.c", tmp_path / "discovery"
    probe.write_text(program)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function",
                    str(probe), "--output=" + str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_lan_kind_is_retained_and_older_reserved_bytes_are_ignored(tmp_path):
    source = (ROOT / "source/networking/network_client_manager.c").read_text()
    declarations = "\n".join(function(source, "struct " + name + "\n") + ";" for name in (
        "transport_key_id", "transport_key", "transport_xnaddr", "network_advertised_game",
        "message_server_game_advertise"))
    end = source.index("} network_game_client_advertised_versions")
    start = source.rindex("static struct\n", 0, end)
    declarations += "\n" + source[start:source.index(";", end) + 1]
    production = function(source, "static boolean add_advertised_game(\n\tstruct network_advertised_game *available_games,\n\tstruct message_server_game_advertise *advertisement)\n{")
    fixture = (ROOT / "port/ios/tests/network_advertisement_probe.c").read_text()
    program = fixture.replace("/* DECLARATIONS */", declarations).replace("/* PRODUCTION */", production)
    program = program.replace('"halo_port_limits.h"',
                              '"' + str(ROOT / "port/linux/include/halo_port_limits.h") + '"')
    probe, binary = tmp_path / "advertisement.c", tmp_path / "advertisement"
    probe.write_text(program)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function",
                    str(probe), "--output=" + str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_browser_names_the_advertised_mode_before_join(tmp_path):
    source = (ROOT / "port/linux/game/menu_functions.c").read_text()
    declarations = function(source, "static wchar_t const *game_type_name(long engine_type)")
    if "static void advertised_game_type_text(" in source:
        declarations += "\n" + function(source, "static void advertised_game_type_text(")
    browser = function(source, "static void browser_update(struct widget_instance *list)\n{")
    start = browser.index("\t\tgame = multiplayer.games[index];")
    end = browser.index('\t\tvisible_set(named(row, "server_item_classic", 0), FALSE);', start)
    rows = browser[start:end]
    declarations += "\n" + function(source, "static short browser_row_index(struct widget_instance *row)")
    declarations += "\n" + function(source, "boolean ui_widget_port_browser_row_unavailable(")
    renderer = (ROOT / "source/interface/ui_widget.c").read_text()
    start = renderer.index("\tif (ui_widget_port_browser_row_unavailable(widget))")
    tint = renderer[start:renderer.index(";", start) + 1]
    fixture = (ROOT / "port/ios/tests/network_browser_probe.c").read_text()
    program = fixture.replace("/* DECLARATIONS */", declarations).replace("/* ROW */", rows)
    program = program.replace("/* TINT */", tint)
    program = program.replace('"halo_network_content.h"',
                              '"' + str(ROOT / "port/linux/include/halo_network_content.h") + '"')
    probe, binary = tmp_path / "browser.c", tmp_path / "browser"
    probe.write_text(program)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function",
                    str(probe), "--output=" + str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
