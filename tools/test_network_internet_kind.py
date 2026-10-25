"""Run the production internet listing parser and browser metadata binding.

A missing stamp read or compatibility gate admits unsupported games. Literal
records follow the permitted format description, independent of listing_make.
"""
from pathlib import Path
import subprocess
from test_network_discovery import function

ROOT = Path(__file__).resolve().parents[1]


def test_internet_kind_and_malformed_listings(tmp_path):
    source = (ROOT / "port/linux/src/p2p_lobby.c").read_text()
    declarations = "\n".join(function(source, "struct " + name + "\n") + ";"
                             for name in ("listing", "game", "tombstone", "queued"))
    production = "\n".join(function(source, signature) for signature in (
        "static unsigned long get_long(", "static int get_text(",
        "static int listing_read(", "static void listing_take("))
    fixture = (ROOT / "port/ios/tests/network_internet_kind_probe.c").read_text()
    program = fixture.replace("/* DECLARATIONS */", declarations).replace("/* PRODUCTION */", production)
    probe, binary = tmp_path / "internet.c", tmp_path / "internet"
    probe.write_text(program)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function",
                    "-iquote", str(ROOT / "port/linux/src"), "-iquote", str(ROOT / "port/linux/include"),
                    str(probe), "--output=" + str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_internet_browser_shows_kind_reason(tmp_path):
    source = (ROOT / "port/linux/game/menu_functions.c").read_text()
    update = function(source, "static void lobby_browser_update(struct widget_instance *list)\n{")
    start = update.index("\t\t\tif (halo_network_kind_reason(game->kind))")
    # Keep the production ticker decision; formatting and widgets are covered
    # by the existing browser probe, so capture only its rendered text here.
    block = update[start:update.index("\n\t\t}\n\t\telse\n\t\t\ttext[0] = 0;", start)]
    fixture = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "p2p.h"
#include "halo_network_version.h"
#include "halo_network_content.h"
#define NUMBEROF(a) (sizeof(a) / sizeof((a)[0]))
static long player_capacity(void) { return 0; }
static int player_capacity_exceeded(long players) { (void)players; return 0; }
static unsigned long ustrlen(wchar_t const *text) { return wcslen(text); }
static void usnprintf(wchar_t *out, int count, wchar_t const *format, ...)
{
    va_list args;
    va_start(args, format);
    if (!wcscmp(format, L"%hs"))
        swprintf(out, count, L"%s", va_arg(args, char const *));
    else
        wcscpy(out, L"other ticker");
    va_end(args);
}
int main(void)
{
    struct p2p_listing row = {0}, *game = &row;
    wchar_t text[256], gametype[] = L"Custom variant";
    for (unsigned kind = 1; kind <= 255; kind++)
    {
        /* SET_KIND */
        row.joinable = 0;
        /* TICKER */
        char actual[256];
        wcstombs(actual, text, sizeof(actual));
        assert(!strcmp(actual, halo_network_kind_reason(kind)));
    }
    return 0;
}
'''
    fixture = '#include <stdlib.h>\n' + fixture
    program = fixture.replace("/* TICKER */", block).replace(
        "/* SET_KIND */", "row.kind = kind;")
    probe, binary = tmp_path / "ticker.c", tmp_path / "ticker"
    probe.write_text(program)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function",
                    "-iquote", str(ROOT / "port/linux/src"), "-iquote", str(ROOT / "port/linux/include"),
                    str(probe), "--output=" + str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_internet_kind_labels_match_lan(tmp_path):
    source = (ROOT / "port/linux/game/menu_functions.c").read_text()
    update = function(source, "static void lobby_browser_update(struct widget_instance *list)\n{")
    start = update.index("\t\tif (game->kind)") if "\t\tif (game->kind)" in update else update.index("\t\tgame_kind_type_text(")
    block = update[start:update.index('\t\tusnprintf(text, ROW_TEXT_LENGTH - 1, L"%d/%d"', start)]
    declarations = function(source, "static wchar_t const *game_type_name(")
    if "static void game_kind_type_text(" in source:
        declarations += "\n" + function(source, "static void game_kind_type_text(")
    fixture = (ROOT / "port/ios/tests/network_browser_probe.c").read_text().split("/* DECLARATIONS */")[0]
    program = fixture + declarations + r'''
static void render_internet(unsigned kind, int engine)
{
    struct { unsigned kind; int engine_type; } data = {kind, engine}, *game = &data;
    struct widget_instance *row = NULL;
    wchar_t text[ROW_TEXT_LENGTH];
    /* LABEL */
}
int main(void)
{
    unsigned kinds[] = {0, 1, 2, 3, 0x80, 0x81, 0x82, 0x83, 4, 0xff};
    wchar_t const *labels[] = {L"CTF", L"SWAT", L"INFECTION", L"GUN GAME",
        L"HC CTF", L"HC SWAT", L"HC INFECTION", L"HC GUN GAME", L"UNSUPPORTED", L"UNSUPPORTED"};
    for (unsigned i = 0; i < NUMBEROF(kinds); i++)
    {
        render_internet(kinds[i], 1);
        assert(!wcscmp(type.text, labels[i]));
    }
    for (int engine = 0; engine <= 5; engine++)
    {
        render_internet(0, engine);
        assert(!wcscmp(type.text, engine == 0 ? L"CO-OP" : engine_names[engine]));
        render_internet(0x80, engine);
        wchar_t expected[ROW_TEXT_LENGTH];
        swprintf(expected, ROW_TEXT_LENGTH, L"HC %ls", engine == 0 ? L"CO-OP" : engine_names[engine]);
        assert(!wcscmp(type.text, expected));
    }
    return 0;
}
'''
    probe, binary = tmp_path / "labels.c", tmp_path / "labels"
    probe.write_text(program.replace("/* LABEL */", block))
    subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function",
                    "-Wno-unused-variable", "-iquote", str(ROOT / "port/linux/include"),
                    str(probe), "--output=" + str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
