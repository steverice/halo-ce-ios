/* The shader-list ABI is independent of replay guest-code identity. */
#pragma once
#include <stddef.h>
#define HOST_SHADER_PACK_COMPATIBILITY "shader-lists-2"
#define HOST_SHADER_PACK_MAX_MAPS 32
#define HOST_SHADER_PACK_MAX_FILE_BYTES (4 * 1024 * 1024)
#define HOST_SHADER_PACK_MAX_TOTAL_BYTES (8 * 1024 * 1024)
#define HOST_SHADER_PACK_MAX_ENVELOPE_BYTES (16 * 1024 * 1024)
#define HOST_SHADER_PACK_MAX_INDEX_BYTES 65536
#define HOST_SHADER_PACK_MAX_COMPRESSED_FILE_BYTES (4 * 1024 * 1024)
#define HOST_SHADER_PACK_MAX_COMPRESSED_TOTAL_BYTES (8 * 1024 * 1024)
#define HOST_SHADER_LIST_MAX_RECORDS 8192
/* Both lists require complete map-local dependencies; the union rejects conflicting definitions.
   Returned canonical, deduplicated text belongs to the caller. NULL additions validates the base only. */
int host_shader_list_merge(const char *base, size_t base_size, const char *additions, size_t additions_size,
    char **merged, size_t *merged_size);
int host_shader_pack_validate(const char *folder, const char *bundled, void (*log)(const char *));
int host_shader_map_name_valid(const char *name);

char *host_shader_list_read_file(const char *path, size_t *size);
