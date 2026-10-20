"""A retained shadow target remains observable after a benchmark run resets its counters."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


def test_recent_shadow_target_hit_after_observation_reset(tmp_path):
    device = (ROOT / "port/linux/src/d3d8_device.c").read_text()
    actual = "\n".join((
        section(device, "void halo_benchmark_renderer_observations_reset(",
            "void halo_benchmark_renderer_observations("),
        section(device, "static void benchmark_observe_shadow_target(",
            "static struct render_target_entry *render_target_get_layer("),
        section(device, "#define RECENT_RENDER_TARGET_COUNT 4", "/* a layer whose target"),
    ))
    source = tmp_path / "shadow_cache.c"
    source.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef unsigned long DWORD;
typedef int BOOL;
#define HALO_STEREO_LAYER_MONO 0
#define HALO_STEREO_LAYER_NATIVE 1
#define HALO_STEREO_LAYER_HUD 2
#define HALO_STEREO_LAYER_UI 3
#define HALO_STEREO_LAYER_HUD_GROUP 4
#define HALO_HUD_GROUP_NONE 0
#define D3DFMT_R5G6B5 100
#define D3DFMT_LIN_R5G6B5 101
typedef struct { DWORD Data, Format, Size; } D3DSurface;
struct render_target_entry { struct { unsigned long width, height, allocated_width, texture; } target; };
static struct { long render_target_lookups, render_target_recent; } stats;
static struct { D3DSurface back_buffer; } device;
static float screen_scale[2] = {1,1};
static int native_scene;
static long benchmark_shadow_target_observed, benchmark_sampler_max_observed;
struct halo_stereo_frame { int eye_count; };
static struct halo_stereo_frame frame;
static struct render_target_entry target;
static int layer_calls;
static struct halo_stereo_frame *halo_stereo_frame(void) { return &frame; }
static long halo_screen_width(void) { return 640; }
static int native_hud_active(void) { return 0; }
static DWORD surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth) {
    *width=128; *height=128; *depth=0; return surface->Format;
}
static struct render_target_entry *render_target_get_layer(const D3DSurface *surface, int layer) {
    (void)surface; (void)layer; layer_calls++; return &target;
}
static int render_target_layer(void) { return 0; }
static int halo_stereo_ui_span(void) { return 0; }
static int halo_stereo_hud_split(void) { return 0; }
static int halo_hud_group_current(void) { return 0; }
''' + actual + r'''
int main(void) {
    D3DSurface shadow = {1,D3DFMT_R5G6B5,1};
    target.target.width=128; target.target.height=128;
    target.target.allocated_width=512; target.target.texture=1;
    recent_render_targets_forget();
    assert(render_target_get(&shadow)==&target && layer_calls==1);
    benchmark_shadow_target_observed=512;
    halo_benchmark_renderer_observations_reset();
    assert(benchmark_shadow_target_observed==0);
    assert(render_target_get(&shadow)==&target && layer_calls==1 && stats.render_target_recent==1);
    assert(benchmark_shadow_target_observed==512);
    target.target.texture=0; halo_benchmark_renderer_observations_reset();
    assert(render_target_get(&shadow)==&target && benchmark_shadow_target_observed==0);
}
''')
    binary = tmp_path / "shadow_cache"
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
