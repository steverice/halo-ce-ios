"""Graphics reports use the guest's resolved typed display settings, never a raw config dump."""
from pathlib import Path
import json
import os
import re
import sys
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_resolved_graphics_obeys_actual_profile_chosen_environment_and_context(tmp_path):
    # Substitute only SDL file I/O with real stdio, preserving the resolver and its on-disk origins.
    headers = tmp_path / "SDL3"; headers.mkdir()
    (headers / "SDL.h").write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *SDL_GetBasePath(void) { return ""; }
static void *SDL_LoadFile(const char *path, size_t *size) {
    FILE *f = fopen(path,"rb"); if (!f) return NULL;
    if (fseek(f,0,SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f); rewind(f); if (n < 0) { fclose(f); return NULL; }
    char *p = malloc((size_t)n+1); if (!p) { fclose(f); return NULL; }
    *size = fread(p,1,(size_t)n,f); fclose(f); p[*size] = 0; return p;
}
static int SDL_SaveFile(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path,"wb"); if (!f) return 0;
    int valid = fwrite(data,1,size,f) == size; fclose(f); return valid;
}
#define SDL_free free
''')
    source = tmp_path / "capture.c"
    source.write_text(r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __HALO_LINUX_PLATFORM_H
void platform_log(const char *format, ...) { (void)format; }
static int external;
int host_external_active(void) { return external; }
#include "port_config.c"
#include "tomlc17.c"
int main(int argc, char **argv) {
    assert(argc == 3); setenv("HALO_DATA_ROOT",argv[1],1);
    setenv("HALO_DEVICE_PROFILE_VALUES","display.render_scale\t0.75\tclass Test\n"
        "display.render_scale\t0.5\tclass Test\tmenu\n"
        "display.render_scale\t0.625\tclass Test\t\texternal\n"
        "display.render_scale\t0.4\tclass Test\tmenu\texternal\n",1);
    if (!strcmp(argv[2],"locale")) { (void)config_boolean("debug.settings_dump"); assert(setlocale(LC_NUMERIC,"fr_FR.UTF-8")); }
    external = !strcmp(argv[2],"external");
    if (!strcmp(argv[2],"environment")) setenv("HALO_RENDER_SCALE","0.9",1);
    char text[CONFIG_GRAPHICS_MAXIMUM_BYTES];
    assert(config_graphics_settings_json(CONFIG_SCENE_MENU,text,sizeof(text)));
    puts(text);
    char escaped[64]; struct graphics_json encoded = {escaped,sizeof(escaped),0,1};
    graphics_string(&encoded,"quote\"slash\\\n");
    assert(encoded.valid && !strcmp(escaped,"\"quote\\\"slash\\\\\\u000a\""));
    long scale = config_setting_index("display.render_scale");
    config_states[scale].source = _source_file; config_values[scale].real = NAN;
    assert(!config_graphics_settings_json(CONFIG_SCENE_MENU,text,sizeof(text)) && !text[0]);
    config_values[scale].real = 0.5;
    long menus = config_setting_index("display.menus");
    char oversized[CONFIG_GRAPHICS_MAXIMUM_STRING_BYTES+2]; memset(oversized,'x',sizeof(oversized)-1); oversized[sizeof(oversized)-1] = 0;
    free(config_values[menus].string); config_values[menus].string = strdup(oversized); config_states[menus].source = _source_file;
    assert(!config_graphics_settings_json(CONFIG_SCENE_MENU,text,sizeof(text)) && !text[0]);
    free(config_values[menus].string); config_values[menus].string = strdup("classic");
    char small[8] = "unsafe";
    assert(!config_graphics_settings_json(CONFIG_SCENE_MENU,small,sizeof(small)) && !small[0]);
    return 0;
}
''')
    binary = tmp_path / "capture"
    subprocess.run(["cc", "-O1", "-DHALO_ILP32=1", "-DHALO_IOS=1", "-I", str(ROOT / "port/linux/src"),
        "-I", str(ROOT / "port/third_party/tomlc17"), "-I", str(tmp_path), str(source), "-lm", "-o", str(binary)], check=True)
    cases = [("profile", None, 0.5), ("external", None, 0.4),
             ("chosen", '[display]\nrender_scale=0.8\n', 0.8),
             ("environment", '[display]\nrender_scale=0.8\n', 0.9)]
    if sys.platform == "darwin":
        cases.append(("locale", None, 0.5))
    for mode, config, expected in cases:
        folder = tmp_path / mode; folder.mkdir()
        if config:
            (folder / "config.toml").write_text(config)
        env = os.environ.copy(); env.pop("HALO_RENDER_SCALE",None)
        result = subprocess.run([str(binary),str(folder),mode],env=env,capture_output=True,text=True,check=True)
        snapshot = json.loads(result.stdout)
        values = snapshot["values"]
        assert values["display.render_scale"] == expected
        assert type(values["display.native_hud"]) is bool
        assert type(values["display.shadow_map_size"]) is int
        assert type(values["display.render_quality"]) is float
        assert values["display.upscaler"] in ("bilinear","metalfx")
        required = {"display.anti_aliasing", "display.lod_scale", "display.interpolation", "display.frame_pacing",
                    "display.vsync", "display.frame_rate", "display.frame_repeat", "display.effect_resolution",
                    "display.shadow_batching", "display.decal_batching", "display.compressed_textures",
                    "display.upscaled_textures", "display.native_hud", "display.high_res_hud", "display.high_res_text",
                    "display.stereo", "display.foveation", "display.render_quality"}
        assert required <= values.keys()
        assert snapshot["scene"] == "menu"
        safe = set(re.findall(r'X\("([^"]+)"\)', (ROOT / "port/runtime/graphics_capture.h").read_text()))
        assert all(key.startswith("display.") or key in safe for key in values)
        assert "network.online" not in values and "input.look_invert_pitch" not in values
        assert len(result.stdout.encode()) < 32768
