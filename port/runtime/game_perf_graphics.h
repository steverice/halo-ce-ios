/* Apache-2.0. Fixed public graphics context; excludes private configuration. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define GAME_PERF_GRAPHICS_MAX_KEYS 128
#define GAME_PERF_GRAPHICS_NAME_BYTES 48
#define GAME_PERF_GRAPHICS_STRING_BYTES 80
enum game_perf_graphics_type {
    GAME_PERF_GRAPHICS_BOOLEAN = 1,
    GAME_PERF_GRAPHICS_INTEGER,
    GAME_PERF_GRAPHICS_REAL,
    GAME_PERF_GRAPHICS_STRING
};
struct game_perf_graphics_value {
    char name[GAME_PERF_GRAPHICS_NAME_BYTES];
    uint32_t type;
    uint32_t reserved;
    union {
        int32_t boolean;
        int64_t integer;
        double real;
        char string[GAME_PERF_GRAPHICS_STRING_BYTES];
    } value;
};
struct game_perf_graphics_snapshot {
    uint32_t schema;
    uint32_t scene;
    uint64_t configuration_revision;
    uint32_t external_output;
    uint32_t count;
    uint32_t valid;
    uint32_t reserved;
    struct game_perf_graphics_value entries[GAME_PERF_GRAPHICS_MAX_KEYS];
};
/* Call on the settings/game thread after initial configuration loading, with no concurrent writes.
   Copies cached allowlisted values without allocation, formatting, I/O, or locks. Invalid captures
   have valid=0 and count=0, including initialization, unknown scenes, oversize and nonfinite values. */
int config_graphics_settings_snapshot(int scene, struct game_perf_graphics_snapshot *snapshot);

#if defined(__cplusplus)
#define GAME_PERF_GRAPHICS_ASSERT static_assert
#else
#define GAME_PERF_GRAPHICS_ASSERT _Static_assert
#endif
GAME_PERF_GRAPHICS_ASSERT(offsetof(struct game_perf_graphics_value, value) == 56, "graphics value ABI offset");
GAME_PERF_GRAPHICS_ASSERT(sizeof(struct game_perf_graphics_value) == 136, "graphics value ABI size");
GAME_PERF_GRAPHICS_ASSERT(offsetof(struct game_perf_graphics_snapshot, entries) == 32, "graphics snapshot ABI offset");
GAME_PERF_GRAPHICS_ASSERT(sizeof(struct game_perf_graphics_snapshot) == 17440, "graphics snapshot ABI size");
#undef GAME_PERF_GRAPHICS_ASSERT
