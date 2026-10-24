"""Stock regional maps reuse warming lists without acquiring upload aliases."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_stock_warming_fallback_keeps_fingerprint_identity(tmp_path):
    probe = tmp_path / "warming.c"
    probe.write_text(r'''
#include <assert.h>
#include "host_shader_official_maps.h"
int main(void) {
    const char *unknown = "0000000000000000000000000000000000000000000000000000000000000000";
    for (size_t i = 0; i < sizeof(host_shader_official_maps) / sizeof(host_shader_official_maps[0]); ++i) {
        const struct host_shader_official_map *map = &host_shader_official_maps[i];
        assert(!strcmp(host_shader_warming_alias(unknown, map->alias), map->alias));
        assert(!strcmp(host_shader_warming_alias(NULL, map->alias), map->alias));
        assert(!strcmp(host_shader_warming_alias(map->fingerprint, "renamed"), map->alias));
    }
    assert(!host_shader_official_alias(unknown));
    assert(!host_shader_warming_alias(unknown, "community"));
    assert(!host_shader_warming_alias(unknown, "../a10"));
    assert(!host_shader_warming_alias(unknown, NULL));
    return 0;
}
''')
    binary = tmp_path / "warming"
    subprocess.run(["cc", "-Wall", "-Werror", "-I", str(ROOT / "port/ios/host"),
                    str(probe), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
