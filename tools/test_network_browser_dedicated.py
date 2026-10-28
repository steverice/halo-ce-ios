"""Exercise dedicated-first ordering and the shared browser count formatter."""
from pathlib import Path
import subprocess
from test_network_discovery import function

ROOT = Path(__file__).resolve().parents[1]


def test_dedicated_order_and_count(tmp_path):
    menu = (ROOT / 'port/linux/game/menu_functions.c').read_text()
    lobby = (ROOT / 'port/linux/src/p2p_lobby.c').read_text()
    production = function(lobby, 'static int listing_order(')
    production += '\n' + function(menu, 'static void browser_game_count_text(')
    production += '\n' + function(menu, 'static void browser_dedicated_first(')
    fixture = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "p2p.h"
#include <stdarg.h>
/* the game's usnprintf reads %s as a wide string; Apple's swprintf reads it as a narrow one */
static void usnprintf(wchar_t *text, size_t count, wchar_t const *format, ...) {
    wchar_t wide[128]; size_t n = 0;
    for (; *format && n < 125; format++) {
        wide[n++] = *format;
        if (*format == L'%' && format[1] == L's') wide[n++] = L'l';
    }
    wide[n] = 0;
    va_list arguments; va_start(arguments, format); vswprintf(text, count, wide, arguments); va_end(arguments);
}
#define ROW_TEXT_LENGTH 96
struct advertised_game { int dedicated; };
static struct { struct advertised_game *games[8]; int game_count; } multiplayer;
static int advertised_dedicated(struct advertised_game *game) { return game->dedicated; }
/* PRODUCTION */
int main(void) {
    struct p2p_listing games[4] = {
        {.name="popular", .joinable=1, .player_count=12},
        {.name="dedicated", .dedicated=1, .joinable=1},
        {.name="unavailable", .dedicated=1},
        {.name="other", .joinable=1, .player_count=1}};
    wchar_t text[96];
    struct advertised_game a={0}, b={1}, c={0}, d={1};
    qsort(games, 4, sizeof(*games), listing_order);
    assert(!strcmp(games[0].name,"dedicated"));
    assert(!strcmp(games[1].name,"unavailable"));
    assert(!strcmp(games[2].name,"popular"));
    multiplayer.games[0]=&a; multiplayer.games[1]=&b;
    multiplayer.games[2]=&c; multiplayer.games[3]=&d; multiplayer.game_count=4;
    browser_dedicated_first();
    assert(multiplayer.games[0]==&b && multiplayer.games[1]==&d);
    assert(multiplayer.games[2]==&a && multiplayer.games[3]==&c);
    browser_game_count_text(text, L"Games", 4, 2);
    assert(!wcscmp(text,L"Games 4 (2 dedicated)"));
    browser_game_count_text(text, L"Games", 0, 0);
    assert(!wcscmp(text,L"Games 0"));
    browser_game_count_text(text, L"Games", 3, 0);
    assert(!wcscmp(text,L"Games 3"));
    return 0;
}
'''
    # Halo uses %s for a wide string; the native probe uses %ls.
    production = production.replace('L"%s %d (', 'L"%ls %d (')
    probe, binary = tmp_path / 'browser.c', tmp_path / 'browser'
    probe.write_text(fixture.replace('/* PRODUCTION */', production))
    subprocess.run(['clang', '-std=c11', '-Wall', '-Werror', '-iquote',
                    str(ROOT / 'port/linux/src'), str(probe), '--output=' + str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_dedicated_art_regenerates_from_empty_output(tmp_path):
    """Menu regeneration removes svg output before drawing any frames."""
    import ast
    import base64
    import shutil

    source = ROOT / 'port/assets/menus/originals/dedicated.svg'
    icons = ['dedicated.svg', 'player_hosted.svg', 'delta_listed.svg', 'lan_game.svg']
    expected_by_frame = [(ROOT / 'port/assets/menus/originals' / name).read_bytes() for name in icons]
    assert len(set(expected_by_frame)) == 4
    expected = expected_by_frame[0]
    copies = tmp_path / 'svg'
    copies.mkdir()
    relative = 'shell/main_menu/multiplayer_type_select/join_game/dedicated_icon'
    module = ast.parse((ROOT / 'tools/ce_menus.py').read_text())
    art = next(node for node in module.body if isinstance(node, ast.ClassDef) and node.name == 'Art')
    draw = next(node for node in art.body if isinstance(node, ast.FunctionDef) and node.name == 'draw')
    draw.decorator_list = []
    emitted = []
    png = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a5WQAAAAASUVORK5CYII=')

    class ImageStub:
        @staticmethod
        def fromarray(array, mode):
            assert array == [[[40, 150, 255, 255]]] and mode == 'RGBA'
            return ImageStub()

        def save(self, path, optimize):
            assert optimize
            path.write_bytes(png)

    def render(svg, scale):
        assert svg.read_bytes() == expected_by_frame[len(emitted)]
        assert scale == 4
        emitted.append(svg)
        return [[[40, 150, 255, 255]]]

    namespace = {'Path': Path, 'shutil': shutil, 'SVG_COPIES': copies, 'SERVER_KIND_ICONS': icons,
                 'MENUS': ROOT / 'port/assets/menus', 'SCALE': 4,
                 'Image': ImageStub, 'bleed': lambda pixels: pixels, 'render_svg': render}
    exec(compile(ast.Module(body=[draw], type_ignores=[]), '<Art.draw>', 'exec'), namespace)
    instance = type('ArtFixture', (), {'svgs': []})()
    for index in range(4):
        output = tmp_path / 'png' / f'dedicated_icon__{index}.png'
        namespace['draw'](instance, relative, index, 4, 16, 16, output, [])
        assert output.read_bytes() == png
        assert (copies / f'{relative}__{index}.svg').read_bytes() == expected_by_frame[index]
    assert len(emitted) == 4 and instance.svgs == emitted
    assert source.read_bytes() == expected


def test_server_kind_classification_and_icon_frames(tmp_path):
    menu = (ROOT / 'port/linux/game/menu_functions.c').read_text()
    lobby = (ROOT / 'port/linux/src/p2p_lobby.c').read_text()
    production = function(lobby, 'enum p2p_server_kind p2p_listing_server_kind(')
    production += '\n' + function(menu, 'static int server_kind_frame(')
    fixture = r'''
#include <assert.h>
#include <stddef.h>
#include "p2p.h"
/* PRODUCTION */
int main(void) {
    struct p2p_listing player = {0}, dedicated = {.dedicated=1}, master = {.from_master=1},
        delta = {.from_delta=1}, delta_dedicated_flag = {.from_delta=1, .dedicated=1};
    assert(p2p_listing_server_kind(&player) == P2P_SERVER_PLAYER);
    assert(p2p_listing_server_kind(&dedicated) == P2P_SERVER_DEDICATED);
    assert(p2p_listing_server_kind(&master) == P2P_SERVER_DEDICATED);
    assert(p2p_listing_server_kind(&delta) == P2P_SERVER_DELTA);
    assert(p2p_listing_server_kind(&delta_dedicated_flag) == P2P_SERVER_DEDICATED);
    assert(p2p_listing_server_kind(NULL) == P2P_SERVER_UNKNOWN);
    assert(server_kind_frame(P2P_SERVER_DEDICATED) == 0 && server_kind_frame(P2P_SERVER_PLAYER) == 1);
    assert(server_kind_frame(P2P_SERVER_DELTA) == 2 && server_kind_frame(P2P_SERVER_LAN) == 3);
    return 0;
}
'''
    probe, binary = tmp_path / 'kind.c', tmp_path / 'kind'
    probe.write_text(fixture.replace('/* PRODUCTION */', production))
    subprocess.run(['clang', '-std=c11', '-Wall', '-Werror', '-iquote',
                    str(ROOT / 'port/linux/src'), str(probe), '--output=' + str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
