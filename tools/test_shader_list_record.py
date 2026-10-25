"""The local shader miss record stays on unless an active observation generation replaces it.

A benchmark, replay or fixed-timestep run suppresses shader observations (its
generation is 0), and the benchmark's per-run shader capture reads
`shader-lists-missed/<map>.txt`, so those runs must keep writing it.
"""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/linux/src/d3d8_device.c"


def function(source, signature):
    """The definition of `signature`, skipping any prototype of it."""
    start = source.index(signature)
    while source[source.index(")", start) + 1] == ";":
        start = source.index(signature, start + 1)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_record_written_unless_an_active_generation_replaces_it():
    source = SOURCE.read_text()
    actual = "\n".join(function(source, signature) for signature in (
        "static void shader_observation_submit(",
        "static int shader_observation_replaces_record(",
        "static BOOL shader_list_recorded(",
        "static void shader_list_recorded_add(",
        "static void shader_list_recorded_load(",
        "static void shader_list_made(",
        "static void shader_list_vertex_made(",
    ))
    probe = r'''
#define HALO_ILP32 1
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
typedef unsigned int gpu_shader;
enum { SHADER_OBSERVATION_VERTEX = 1 };
struct shader_observation {
    int kind, metal, warming, variant;
    uint64_t vertex_hash;
    unsigned long packed_mask;
};
struct nv2a_pixel_shader_key { unsigned char byte; };
struct shader_list_name {
    unsigned char kind, variant;
    unsigned long packed_mask;
    uint64_t hash;
    const struct nv2a_pixel_shader_key *pixel_key;
};
struct vertex_shader_object {
    uint64_t program_hash;
    gpu_shader shader[2];
    unsigned long shader_packed_mask[2];
};
static struct {
    char map[64];
    const char *record_directory;
    int record_checked;
    char record_map[64];
    uint64_t *recorded;
    unsigned long recorded_count, recorded_capacity;
} shader_list;
static struct { int msl; } shader_dialect = { 1 };
static int shader_list_warming;
static const char *record_setting;
static uint32_t observation_generation;
static int observation_capture, observation_developer_record;
static unsigned int appended;
static struct shader_list_name names[4];
static struct shader_list_name *shader_list_name(gpu_shader shader) { return shader ? &names[shader] : NULL; }
static void host_shader_observation_append(uint32_t generation, struct shader_observation *event) {
    (void)event; if (generation) appended++;
}
static uint64_t hash64(const void *data, size_t size) {
    const unsigned char *bytes = data; uint64_t hash = 1469598103934665603ULL;
    while (size--) hash = (hash ^ *bytes++) * 1099511628211ULL;
    return hash;
}
static void platform_log(const char *format, ...) { (void)format; }
static const char *config_string(const char *name) { (void)name; return record_setting; }
static void posix_make_directory(const char *path) { (void)path; }
''' + actual + r'''
int main(int argc, char **argv) {
    struct vertex_shader_object program = { 0x1111111111111111ULL, { 1, 0 }, { 0x3, 0 } };
    assert(argc == 7);
    record_setting = argv[1];
    observation_capture = atoi(argv[2]);
    observation_generation = (uint32_t)atoi(argv[3]);
    observation_developer_record = atoi(argv[4]);
    snprintf(shader_list.map, sizeof(shader_list.map), "a10");
    shader_list_vertex_made(&program, 0);
    shader_list_vertex_made(&program, 0);
    char path[512]; FILE *file; char line[128] = {0}; int lines = 0;
    snprintf(path, sizeof(path), "%s/a10.txt", record_setting);
    if ((file = fopen(path, "r")) != NULL) {
        while (fgets(line, sizeof(line), file)) lines++;
        fclose(file);
    }
    assert(lines == atoi(argv[5]));
    if (lines) assert(!strcmp(line, "vs 1111111111111111 0 3\n"));
    assert(appended == (unsigned int)atoi(argv[6]));
    printf("PASS: capture %d generation %u explicit %d: %d record lines, %u observations\n",
        observation_capture, observation_generation, observation_developer_record, lines, appended);
    free(shader_list.recorded);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        c_file, binary = temp / "probe.c", temp / "probe"
        c_file.write_text(probe)
        subprocess.run(["xcrun", "clang", "-Wall", "-Werror", "-Wno-unused-function", "-O1",
                        "-fsanitize=address,undefined", str(c_file), "-o", str(binary)], check=True)
        # (capture, generation, explicit record) -> (record lines, observations appended)
        cases = (
            ("no-observation-build", (0, 0, 0), (1, 0)),
            ("suppressed-benchmark", (1, 0, 0), (1, 0)),
            ("active-generation", (1, 5, 0), (0, 2)),
            ("active-generation-explicit-record", (1, 5, 1), (1, 2)),
        )
        for name, (capture, generation, explicit), (lines, observations) in cases:
            records = temp / name
            records.mkdir()
            subprocess.run([str(binary), str(records), str(capture), str(generation), str(explicit),
                            str(lines), str(observations)], check=True)


if __name__ == "__main__":
    test_record_written_unless_an_active_generation_replaces_it()
