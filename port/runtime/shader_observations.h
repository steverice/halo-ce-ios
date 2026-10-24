/* Fixed-width guest/native shader observation ABI. Apache-2.0. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#define SHADER_OBSERVATION_ABI "shader-lists-2"
#define SHADER_OBSERVATION_PIXEL_MAX 512
#define SHADER_OBSERVATION_CAPACITY 512
#define SHADER_OBSERVATION_LINE_MAX 1152
struct shader_observation_generation {
    uint32_t token;
    char fingerprint[65], map_key[64], map_format[32];
    char guest_sha256[65], shader_list_id[160];
};
enum { SHADER_OBSERVATION_VERTEX = 1, SHADER_OBSERVATION_PIXEL, SHADER_OBSERVATION_PIPELINE };
/* Pipeline dependencies are values, never live GPU handles. */
struct shader_observation {
    uint64_t vertex_hash, pixel_hash;
    uint32_t kind, variant, packed_mask, pixel_size;
    uint8_t pixel[SHADER_OBSERVATION_PIXEL_MAX];
    uint8_t blend, source, destination, operation, write_mask, exact_borders, depth;
    uint8_t alpha_source, alpha_destination, attribute_kinds[16];
    uint8_t warming, metal, suppressed, reserved;
};
struct shader_observation_item {
    struct shader_observation_generation generation;
    struct shader_observation descriptor;
};
/* One render producer and one worker consumer. Never reset while either is active. */
struct shader_observation_collector {
    _Atomic uint32_t read, write, dropped;
    struct shader_observation_item items[SHADER_OBSERVATION_CAPACITY];
};
void shader_observations_init(struct shader_observation_collector *collector);
int shader_observations_append(struct shader_observation_collector *collector,
    const struct shader_observation_generation *generation, const struct shader_observation *descriptor);
int shader_observations_take(struct shader_observation_collector *collector, struct shader_observation_item *item);
/* Worker only: outputs the event line and supporting vertex/pixel lines.
   The renderer retains warmed identities; each pipeline copies both dependencies. */
int shader_observations_lines(const struct shader_observation *descriptor,
    char line[SHADER_OBSERVATION_LINE_MAX], char vertex[SHADER_OBSERVATION_LINE_MAX],
    char pixel[SHADER_OBSERVATION_LINE_MAX]);
