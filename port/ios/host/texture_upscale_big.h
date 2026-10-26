/* Large buffers for the upscaler, mapped from the system directly instead of through malloc. Freed blocks of a
megabyte or more otherwise stay in libmalloc's large-block cache, dirty and counted in the app's footprint ("Malloc
Large (empty)" in vmmap: 395 MB after the generator was released on a Mac, about 150 MB on an M1 iPad), and
malloc_zone_pressure_relief does not return them. Small blocks still use malloc. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEXTURE_UPSCALE_BIG_HEADER 64
#define TEXTURE_UPSCALE_BIG_MAPPED ((size_t)1 << 20)

struct texture_upscale_big_header { size_t size; uint32_t mapped, magic; };

static inline void *texture_upscale_big_alloc(size_t bytes)
{
    size_t total = bytes + TEXTURE_UPSCALE_BIG_HEADER;
    int mapped = total >= TEXTURE_UPSCALE_BIG_MAPPED;
    void *block;
    if (total < bytes) return NULL;
    if (mapped) {
        block = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (block == MAP_FAILED) return NULL;
    } else if (!(block = malloc(total))) return NULL;
    struct texture_upscale_big_header *header = (struct texture_upscale_big_header *)block;
    header->size = total; header->mapped = (uint32_t)mapped; header->magic = 0x75707363u;
    return (unsigned char *)block + TEXTURE_UPSCALE_BIG_HEADER;
}

static inline void texture_upscale_big_free(void *pointer)
{
    if (!pointer) return;
    struct texture_upscale_big_header *header =
        (struct texture_upscale_big_header *)((unsigned char *)pointer - TEXTURE_UPSCALE_BIG_HEADER);
    if (header->magic != 0x75707363u) abort();
    if (header->mapped) munmap(header, header->size);
    else free(header);
}

static inline void *texture_upscale_big_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    void *pointer = texture_upscale_big_alloc(count * size);
    struct texture_upscale_big_header *header = pointer ?
        (struct texture_upscale_big_header *)((unsigned char *)pointer - TEXTURE_UPSCALE_BIG_HEADER) : NULL;
    /* (a fresh mapping is already zero) */
    if (header && !header->mapped) memset(pointer, 0, count * size);
    return pointer;
}

/* A file whose buffers never leave it routes every allocation here, after its system headers:
   #define malloc(n) texture_upscale_big_alloc(n), calloc and free the same way */

/* keeps the first bytes, returning the mapping's unused whole pages at once */
static inline void *texture_upscale_big_shrink(void *pointer, size_t bytes)
{
    if (!pointer) return NULL;
    struct texture_upscale_big_header *header =
        (struct texture_upscale_big_header *)((unsigned char *)pointer - TEXTURE_UPSCALE_BIG_HEADER);
    size_t page = (size_t)getpagesize(), keep = bytes + TEXTURE_UPSCALE_BIG_HEADER;
    if (!header->mapped || keep >= header->size) return pointer;
    keep = (keep + page - 1) / page * page;
    size_t end = (header->size + page - 1) / page * page;
    if (keep < end) munmap((unsigned char *)header + keep, end - keep);
    header->size = keep;
    return pointer;
}
