/* Apache-2.0. Arrival hooks only schedule work; normal level loads never wait. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
void host_astc_store_init(const char *data_root);
void host_astc_store_scan(void);
void host_astc_map_arrived(const char *path);
void host_astc_map_will_load(const char *name);
/* A game begins (a level) or the menus open; the worker pauses during play. */
void host_astc_game_started(void);
void astc_store_map_arrived(const char *path);
void astc_store_map_will_load(const char *name);
/* Return zero to leave preparation; committed records resume on the next scan. */
typedef int (*host_astc_progress)(void *context, uint64_t done, uint64_t total);
void host_astc_store_prepare_import(const char *root, host_astc_progress progress, void *context);
/* The original uploaded mip chain, tightly packed RGBA8 in level order.
 * The callback borrows these bytes only until it returns. Material addressing
 * and alpha semantics come from texture_policy's merged catalog decision. */
struct host_texture_source {
    uint64_t hash;
    uint32_t width, height, format, flags, level_count;
    const uint8_t *rgba;
    size_t rgba_size, level_offsets[13];
};
typedef int (*host_texture_source_callback)(void *context, const struct host_texture_source *source);
typedef int (*host_texture_map_callback)(void *context, const char *path);
/* Synchronous, sorted by map filename, then path. Resource-only bitmaps.map is
 * visited through its owning maps. Return 1 complete, 0 canceled, -1 on error. */
int host_texture_source_scan(const char *root, host_texture_map_callback callback, void *context);
int host_texture_source_walk_map(const char *path, host_texture_source_callback callback, void *context);
/* Caller owns an inflated anonymous file, ready to mmap for
 * texture_policy_add_map without retaining two whole-map allocations. Policy
 * parsing currently supports Xbox cache-version-5 maps only. */
FILE *host_texture_source_open_policy_map(const char *path, uint64_t *size, char *error, size_t error_size);
int host_texture_source_is_playing(void);
#ifdef __cplusplus
}
#endif
