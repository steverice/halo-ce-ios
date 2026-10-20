"""Exercise actual sampler and texture selection paths used by graphics reports."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]


def compile_and_run(tmp_path, name, source):
    path = tmp_path / (name + ".c")
    path.write_text(source)
    binary = tmp_path / name
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_sampler_observation_requires_a_mipmapped_linear_stage(tmp_path):
    device = (ROOT / "port/linux/src/d3d8_device.c").read_text()
    actual = section(device, "static long anisotropy_in_force(", "/* ---------- render targets sampled")
    compile_and_run(tmp_path, "samplers", r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef unsigned long DWORD;
typedef int BOOL;
enum { D3DTSS_MINFILTER, D3DTSS_MIPFILTER, D3DTSS_MAGFILTER, D3DTSS_ADDRESSU,
    D3DTSS_ADDRESSV, D3DTSS_ADDRESSW, D3DTSS_MAXMIPLEVEL, D3DTSS_MAXANISOTROPY,
    D3DTSS_MIPMAPLODBIAS, D3DTSS_BORDERCOLOR };
enum { GPU_FILTER_NONE, GPU_FILTER_POINT, GPU_FILTER_LINEAR, GPU_FILTER_ANISOTROPIC };
struct gpu_sampler_state { int min_filter, mip_filter, mag_filter, address_u, address_v, address_w;
    uint32_t max_mip_level, max_anisotropy; float lod_bias; uint32_t border_color; };
static DWORD D3D__TextureState[1][10];
static long benchmark_anisotropy, knob_anisotropy, benchmark_sampler_max_observed;
static int gpu_filter(DWORD value) { return (int)value; }
static int gpu_address(DWORD value) { return (int)value; }
static float dword_to_float(DWORD value) { (void)value; return 0; }
static long config_integer(const char *name) { (void)name; return 1; }
''' + actual + r'''
int main(void) {
    struct gpu_sampler_state sampler;
    D3D__TextureState[0][D3DTSS_MINFILTER]=GPU_FILTER_POINT;
    D3D__TextureState[0][D3DTSS_MIPFILTER]=GPU_FILTER_LINEAR;
    sampler_state_fill(0,1,0,&sampler);
    assert(benchmark_sampler_max_observed==0);
    D3D__TextureState[0][D3DTSS_MINFILTER]=GPU_FILTER_LINEAR;
    sampler_state_fill(0,0,0,&sampler);
    assert(benchmark_sampler_max_observed==0);
    sampler_state_fill(0,1,0,&sampler);
    assert(benchmark_sampler_max_observed==1);
    benchmark_anisotropy=8;
    sampler_state_fill(0,1,0,&sampler);
    assert(benchmark_sampler_max_observed==8 && sampler.min_filter==GPU_FILTER_ANISOTROPIC);
    D3D__TextureState[0][D3DTSS_MINFILTER]=GPU_FILTER_POINT;
    sampler_state_fill(0,1,0,&sampler);
    assert(benchmark_sampler_max_observed==8);
}
''')


def test_replacement_counts_only_a_selected_texture_in_an_open_window(tmp_path):
    suite = (ROOT / "port/linux/src/benchmark_suite.c").read_text()
    textures = (ROOT / "port/linux/src/xbox_textures.c").read_text()
    actual = section(suite, "void benchmark_graphics_texture_replacement_selected(",
        "static int benchmark_graphics_snapshot_json(") + section(textures,
        "static gpu_texture texture_entry_result(", "gpu_texture xgpu_texture_get(")
    compile_and_run(tmp_path, "textures", r'''
#include <assert.h>
#include <stdint.h>
#define HALO_IOS 1
#define TRUE 1
#define GPU_TEXTURE_2D 1
typedef unsigned long gpu_texture;
typedef unsigned long DWORD;
typedef uint32_t D3DCOLOR;
struct xgpu_texture_description { unsigned long levels; int hires; int hires_coverage; int hires_point_threshold; };
struct texture_entry { DWORD data; uint32_t type; struct xgpu_texture_description description;
    int override; gpu_texture texture_override; unsigned long texture_override_levels; gpu_texture texture; };
static int graphics_window_open;
static unsigned long long texture_replacement_selections;
static gpu_texture text_hires_atlas_texture(DWORD data) { (void)data; return 0; }
static gpu_texture menu_art_texture(DWORD data, unsigned long *levels) { (void)data; (void)levels; return 0; }
static gpu_texture hud_hires_override_texture(int value, unsigned long *levels) { (void)value; (void)levels; return 0; }
static int hud_hires_override_coverage(int value) { (void)value; return 0; }
static int hud_hires_override_point_threshold(int value) { (void)value; return 0; }
''' + actual + r'''
int main(void) {
    struct texture_entry entry = {0};
    struct xgpu_texture_description description;
    uint32_t type;
    entry.override=-1; entry.texture=1;
    assert(texture_entry_result(&entry,&type,&description)==1 && texture_replacement_selections==0);
    entry.texture_override=2; entry.texture_override_levels=3;
    assert(texture_entry_result(&entry,&type,&description)==2 && texture_replacement_selections==0);
    graphics_window_open=1;
    assert(texture_entry_result(&entry,&type,&description)==2 && texture_replacement_selections==1);
    assert(description.levels==3);
    texture_replacement_selections=9007199254740991ULL;
    assert(texture_entry_result(&entry,&type,&description)==2 && texture_replacement_selections==9007199254740991ULL);
    texture_replacement_selections=0; entry.texture_override=0;
    assert(texture_entry_result(&entry,&type,&description)==1 && texture_replacement_selections==0);
}
''')
