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
#define usnprintf swprintf
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
    assert(!wcscmp(text,L"Games 0 (0 dedicated)"));
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
    expected = (ROOT / 'port/assets/menus/svg/shell/main_menu/multiplayer_type_select/'
                'join_game/dedicated_icon__0.svg').read_bytes()
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
        assert svg.read_bytes() == expected
        assert scale == 4
        emitted.append(svg)
        return [[[40, 150, 255, 255]]]

    namespace = {'Path': Path, 'shutil': shutil, 'SVG_COPIES': copies,
                 'MENUS': ROOT / 'port/assets/menus', 'SCALE': 4,
                 'Image': ImageStub, 'bleed': lambda pixels: pixels, 'render_svg': render}
    exec(compile(ast.Module(body=[draw], type_ignores=[]), '<Art.draw>', 'exec'), namespace)
    instance = type('ArtFixture', (), {'svgs': []})()
    for index in range(4):
        output = tmp_path / 'png' / f'dedicated_icon__{index}.png'
        namespace['draw'](instance, relative, index, 4, 16, 16, output, [])
        assert output.read_bytes() == png
        assert (copies / f'{relative}__{index}.svg').read_bytes() == expected
    assert len(emitted) == 4 and instance.svgs == emitted
    assert source.read_bytes() == expected
