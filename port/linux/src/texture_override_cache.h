/* One resident override set. Switching or loading a map retires its GPU
handles; entries re-resolve through the new serial on their next draw. */
#ifndef TEXTURE_OVERRIDE_CACHE_H
#define TEXTURE_OVERRIDE_CACHE_H
#include <stdint.h>
#include "gpu.h"
void texture_override_cache_clear(void);
unsigned long texture_override_cache_serial(void);
/* Higher uses only hashes present in current, at exactly twice current's
width and height. A missing/invalid higher falls back to current and sets
fallback; hashes outside the current policy are untouched. */
gpu_texture texture_override_cache_find(const char *current, const char *higher_directory, int higher,
	uint64_t hash, unsigned long width, unsigned long height, unsigned long *levels, int *fallback);
#endif
