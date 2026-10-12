/*
TEXTURE_OVERRIDE_CACHE.H

The replacement textures in GPU memory: debug.texture_override_directory's
files (texture_override.h) read into textures under a byte budget
(texture_replacement_cache.h, which says what is kept and what goes).
debug.texture_compare_directory names a second set; one of the two is
selected, and a texture the second set has no usable file for is drawn from
the first.
*/

#ifndef TEXTURE_OVERRIDE_CACHE_H
#define TEXTURE_OVERRIDE_CACHE_H

#include <stdint.h>
#include "gpu.h"
#include "texture_replacement_cache.h"

/* where a texture's replacement is in the cache, good for one serial of it;
all zero: not looked for yet */
struct texture_override_reference
{
	long index;
	unsigned long serial;
};

/* the budget, in bytes; the first call makes the cache, later ones do nothing */
void texture_override_cache_start(uint64_t budget);
/* NULL until texture_override_cache_start */
struct texture_replacement_cache *texture_override_cache(void);
/* the two sets' folders ("" for none); kept, not copied */
void texture_override_cache_directories(const char *first, const char *second);
/* a map has loaded, with or without upscaled textures for the level */
void texture_override_cache_map_loaded(int enabled);
/* draw the second set (or the first again) from now on */
void texture_override_cache_select(int second);
/* a frame begins: the number of replacements dropped because the level has
not drawn them, and their bytes in freed */
unsigned long texture_override_cache_begin_frame(uint64_t *freed);
/* the system warned of low memory: the bytes dropped */
uint64_t texture_override_cache_memory_warning(void);
/* the selected set's replacement for a texture with this hash and size, and
its level count: the one in memory, else read from its file if this is the
first draw of the texture or it was dropped since, and there is room. 0: draw
the texture as the map has it. fallback is set when the second set is
selected and the first set's replacement stands in */
gpu_texture texture_override_cache_find(struct texture_override_reference *reference, uint64_t hash,
	unsigned long width, unsigned long height, unsigned long *levels, int *fallback);
/* the milliseconds file reads and uploads have taken, and their number,
since the last call */
double texture_override_cache_take_read_milliseconds(unsigned long *reads);

#endif
