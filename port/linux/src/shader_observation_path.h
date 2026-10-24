/* Resolve contribution identity from the cache actually loaded, never its logical tag path. */
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static inline int shader_observation_map_path(const char *logical, const char *retail_directory,
    const char *loaded_custom_path, char *path, size_t capacity)
{
    const char *base = logical, *cursor;
    int length;
    if (!path || !capacity) return 0;
    path[0] = 0;
    if (loaded_custom_path && *loaded_custom_path)
        length = snprintf(path, capacity, "%s", loaded_custom_path);
    else {
        if (!logical || !*logical || !retail_directory || !*retail_directory) return 0;
        for (cursor = logical; *cursor; ++cursor)
            if (*cursor == '\\' || *cursor == '/') base = cursor + 1;
        if (!*base) return 0;
        length = snprintf(path, capacity, "%s%s.map", retail_directory, base);
    }
    if (length < 0 || (size_t)length >= capacity) { path[0] = 0; return 0; }
    return 1;
}
