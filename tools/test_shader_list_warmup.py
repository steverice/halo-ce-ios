"""Run the real captured-list parser with native GPU boundary stubs."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/linux/src/d3d8_device.c"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def pipeline(vertex_hash):
    return (f"pipeline {vertex_hash}.0.0 2222222222222222 "
            "0 1 2 3 15 0 0 0000 1 2")


def entries(vertex_hash, pixel_byte):
    return (f"vs {vertex_hash} 0 0\n"
            f"ps 2222222222222222 {pixel_byte}\n"
            f"{pipeline(vertex_hash)}")


def test_captured_pipelines_warm_from_bundled_and_local_records():
    source = SOURCE.read_text()
    actual = "\n".join(function(source, signature) for signature in (
        "static int hex_digit(",
        "static BOOL hex_bytes(",
        "static struct vertex_shader_object *shader_list_program(",
        "void halo_shader_list_warm(",
    ))
    probe = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define GPU_ATTRIBUTE_COUNT 2
#define GPU_ATTRIBUTE_BGRA8 1
#define GPU_ATTRIBUTE_RGBA8 2
#define SHADER_LIST_RECORD_MAXIMUM (4L * 1024 * 1024)
typedef unsigned int gpu_shader;
struct vertex_shader_object {
    struct vertex_shader_object *next_object;
    uint64_t program_hash;
    void *instructions;
    gpu_shader shader[2];
    unsigned long shader_packed_mask[2];
    unsigned long packed_mask;
};
struct nv2a_pixel_shader_key { unsigned char byte; };
struct gpu_pipeline_description {
    gpu_shader vertex_shader, pixel_shader;
    unsigned char attribute_kinds[GPU_ATTRIBUTE_COUNT];
    uint8_t blend, source, destination, operation, write_mask;
    uint8_t exact_borders, depth, alpha_source, alpha_destination;
};
static struct vertex_shader_object programs[2];
static struct vertex_shader_object *vertex_shader_objects = programs;
static struct { char map[256]; } shader_list;
static struct { int gl_ready; } device;
static struct { int msl; } shader_dialect;
static struct { int vertex_bgra; } device_capabilities;
static int shader_list_warming;
static int warm_enabled = 1;
static const char *record_directory;
static unsigned char *bundled_text;
static uint32_t bundled_size;
static unsigned int vertex_count, pixel_count, pipeline_count;
static const uint64_t pixel_hash = 0x2222222222222222ULL;

static unsigned int read_file(const char *path, unsigned char **bytes) {
    FILE *file = fopen(path, "rb");
    long length;
    if (!file) return 0;
    assert(!fseek(file, 0, SEEK_END));
    length = ftell(file);
    assert(length >= 0);
    rewind(file);
    *bytes = malloc((size_t)length + 1);
    assert(*bytes);
    assert(fread(*bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    return (unsigned int)length;
}
static uint32_t gpu_warm_list_read(const char *map, char *destination, uint32_t size) {
    (void)map;
    if (!destination) return bundled_size;
    assert(size == bundled_size);
    memcpy(destination, bundled_text, size);
    return size;
}
static const char *config_string(const char *key) {
    if (!strcmp(key, "debug.shader_list_record")) return record_directory;
    return "";
}
static int config_boolean(const char *key) {
    return !strcmp(key, "debug.shader_list_warm") && warm_enabled;
}
static double halo_frame_trace_milliseconds(void) { return 0.0; }
static void platform_log(const char *format, ...) { (void)format; }
static void gpu_warm_begin(void) {}
static void gpu_warm_end(void) {}
static gpu_shader vertex_shader_compile(struct vertex_shader_object *program, int variant, unsigned long mask) {
    program->shader[variant] = (gpu_shader)(10 + vertex_count);
    program->shader_packed_mask[variant] = mask;
    vertex_count++;
    return program->shader[variant];
}
static gpu_shader fragment_shader_get(const struct nv2a_pixel_shader_key *key) {
    (void)key;
    pixel_count++;
    return 7;
}
static gpu_shader shader_list_pixel(uint64_t hash) {
    return hash == pixel_hash ? 7 : 0;
}
static void gpu_pipeline_warm(const struct gpu_pipeline_description *description) {
    assert(description->vertex_shader != 0);
    assert(description->pixel_shader == 7);
    pipeline_count++;
}

''' + actual + r'''
int main(int argc, char **argv) {
    unsigned int expected_vertices, expected_pixels, expected_pipelines;
    assert(argc == 6);
    bundled_size = read_file(argv[1], &bundled_text);
    record_directory = argv[2];
    expected_vertices = (unsigned int)strtoul(argv[3], NULL, 10);
    expected_pixels = (unsigned int)strtoul(argv[4], NULL, 10);
    expected_pipelines = (unsigned int)strtoul(argv[5], NULL, 10);
    programs[0].next_object = &programs[1];
    programs[0].program_hash = 0x1111111111111111ULL;
    programs[0].instructions = &programs[0];
    programs[1].program_hash = 0x3333333333333333ULL;
    programs[1].instructions = &programs[1];
    device.gl_ready = shader_dialect.msl = 1;
    halo_shader_list_warm("levels/test/testmap");
    assert(vertex_count == expected_vertices);
    assert(pixel_count == expected_pixels);
    assert(pipeline_count == expected_pipelines);
    printf("PASS: %u vertex, %u pixel, %u pipeline warmups\n", vertex_count, pixel_count, pipeline_count);
    free(bundled_text);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        c_file, binary = temp / "probe.c", temp / "probe"
        c_file.write_text(probe)
        subprocess.run(["xcrun", "clang", "-Wall", "-Werror", "-O1",
                        "-fsanitize=address,undefined", str(c_file), "-o", str(binary)], check=True)

        cases = (
            ("bundled-and-local", f"# bundled\n\n{entries('1111111111111111', '00')}",
             f"# local\n\n{entries('3333333333333333', '01')}", (2, 2, 2)),
            ("bundled-and-local-trailing-newlines",
             f"# bundled\n\n{entries('1111111111111111', '00')}\n",
             f"# local\n\n{entries('3333333333333333', '01')}\n", (2, 2, 2)),
            ("local-only", "", f"# local only\n\n{entries('3333333333333333', '01')}", (1, 1, 1)),
            ("bundled-only", f"# bundled only\n\n{entries('1111111111111111', '00')}", "", (1, 1, 1)),
            ("empty", "", "", (0, 0, 0)),
        )
        for name, bundled, local, expected in cases:
            case = temp / name
            records = case / "records"
            records.mkdir(parents=True)
            bundle_file = case / "bundled.txt"
            bundle_file.write_text(bundled)
            if local:
                (records / "testmap.txt").write_text(local)
            subprocess.run([str(binary), str(bundle_file), str(records), *(str(value) for value in expected)],
                           check=True)


if __name__ == "__main__":
    test_captured_pipelines_warm_from_bundled_and_local_records()
