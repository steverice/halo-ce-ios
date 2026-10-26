/* Large buffers for the upscaler, mapped from the system directly instead of through malloc. Freed blocks of a
megabyte or more otherwise stay in libmalloc's large-block cache, dirty and counted in the app's footprint ("Malloc
Large (empty)" in vmmap: 395 MB after the generator was released on a Mac, about 150 MB on an M1 iPad), and
malloc_zone_pressure_relief does not return them. Small blocks still use malloc.

While the generator runs, freed mappings are kept for the next texture of the same size (up to
TEXTURE_UPSCALE_BIG_CACHE_BYTES), since a fresh mapping faults in every page again (refinement and the end stage took
20 to 25% longer per texture without the reuse on an M1 iPad); texture_upscale_big_trim returns them all, and the
scheduler calls it when it releases the generator. */
#pragma once
#include <os/lock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEXTURE_UPSCALE_BIG_HEADER 64
#define TEXTURE_UPSCALE_BIG_MAPPED ((size_t)1 << 20)
#define TEXTURE_UPSCALE_BIG_CACHE_SLOTS 16
#define TEXTURE_UPSCALE_BIG_CACHE_BYTES ((size_t)192 << 20)
#define TEXTURE_UPSCALE_BIG_MAGIC 0x75707363u

struct texture_upscale_big_header { size_t size; uint32_t mapped, magic; };

/* the freed mappings kept for reuse, one set for the whole process (a weak definition: every file that includes this
   header shares it) */
struct texture_upscale_big_cache
{
    os_unfair_lock lock;
    size_t bytes;
    size_t sizes[TEXTURE_UPSCALE_BIG_CACHE_SLOTS];
    void *blocks[TEXTURE_UPSCALE_BIG_CACHE_SLOTS];
};
__attribute__((weak)) struct texture_upscale_big_cache texture_upscale_big_cache_state = { OS_UNFAIR_LOCK_INIT, 0, {0}, {0} };

static inline size_t texture_upscale_big_pages(size_t bytes)
{
    size_t page = (size_t)getpagesize();
    return (bytes + page - 1) / page * page;
}

/* *fresh: 1 when the block is a new mapping (all zero), 0 when it was reused or came from malloc */
static inline void *texture_upscale_big_alloc_fresh(size_t bytes, int *fresh)
{
    struct texture_upscale_big_cache *cache = &texture_upscale_big_cache_state;
    size_t total = bytes + TEXTURE_UPSCALE_BIG_HEADER;
    int mapped = total >= TEXTURE_UPSCALE_BIG_MAPPED;
    void *block = NULL;
    if (fresh) *fresh = 0;
    if (total < bytes) return NULL;
    if (mapped) {
        total = texture_upscale_big_pages(total);
        os_unfair_lock_lock(&cache->lock);
        for (unsigned i = 0; i < TEXTURE_UPSCALE_BIG_CACHE_SLOTS; i++)
            if (cache->blocks[i] && cache->sizes[i] == total) {
                block = cache->blocks[i]; cache->blocks[i] = NULL; cache->bytes -= total;
                break;
            }
        os_unfair_lock_unlock(&cache->lock);
        if (!block) {
            block = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (block == MAP_FAILED) return NULL;
            if (fresh) *fresh = 1;
        }
    } else if (!(block = malloc(total))) return NULL;
    struct texture_upscale_big_header *header = (struct texture_upscale_big_header *)block;
    header->size = total; header->mapped = (uint32_t)mapped; header->magic = TEXTURE_UPSCALE_BIG_MAGIC;
    return (unsigned char *)block + TEXTURE_UPSCALE_BIG_HEADER;
}

static inline void *texture_upscale_big_alloc(size_t bytes) { return texture_upscale_big_alloc_fresh(bytes, NULL); }

static inline void *texture_upscale_big_calloc(size_t count, size_t size)
{
    int fresh = 0;
    if (size && count > SIZE_MAX / size) return NULL;
    void *pointer = texture_upscale_big_alloc_fresh(count * size, &fresh);
    if (pointer && !fresh) memset(pointer, 0, count * size);
    return pointer;
}

static inline void texture_upscale_big_free(void *pointer)
{
    struct texture_upscale_big_cache *cache = &texture_upscale_big_cache_state;
    if (!pointer) return;
    struct texture_upscale_big_header *header =
        (struct texture_upscale_big_header *)((unsigned char *)pointer - TEXTURE_UPSCALE_BIG_HEADER);
    if (header->magic != TEXTURE_UPSCALE_BIG_MAGIC) abort();
    if (!header->mapped) { free(header); return; }
    size_t size = header->size;
    os_unfair_lock_lock(&cache->lock);
    if (cache->bytes + size <= TEXTURE_UPSCALE_BIG_CACHE_BYTES)
        for (unsigned i = 0; i < TEXTURE_UPSCALE_BIG_CACHE_SLOTS; i++)
            if (!cache->blocks[i]) {
                cache->blocks[i] = header; cache->sizes[i] = size; cache->bytes += size;
                header = NULL;
                break;
            }
    os_unfair_lock_unlock(&cache->lock);
    if (header) munmap(header, size);
}

/* unmaps every kept mapping */
static inline void texture_upscale_big_trim(void)
{
    struct texture_upscale_big_cache *cache = &texture_upscale_big_cache_state;
    void *blocks[TEXTURE_UPSCALE_BIG_CACHE_SLOTS];
    size_t sizes[TEXTURE_UPSCALE_BIG_CACHE_SLOTS];
    os_unfair_lock_lock(&cache->lock);
    memcpy(blocks, cache->blocks, sizeof(blocks)); memcpy(sizes, cache->sizes, sizeof(sizes));
    memset(cache->blocks, 0, sizeof(cache->blocks)); cache->bytes = 0;
    os_unfair_lock_unlock(&cache->lock);
    for (unsigned i = 0; i < TEXTURE_UPSCALE_BIG_CACHE_SLOTS; i++)
        if (blocks[i]) munmap(blocks[i], sizes[i]);
}

/* keeps the first bytes, returning the mapping's unused whole pages at once */
static inline void *texture_upscale_big_shrink(void *pointer, size_t bytes)
{
    if (!pointer) return NULL;
    struct texture_upscale_big_header *header =
        (struct texture_upscale_big_header *)((unsigned char *)pointer - TEXTURE_UPSCALE_BIG_HEADER);
    size_t keep = texture_upscale_big_pages(bytes + TEXTURE_UPSCALE_BIG_HEADER);
    if (!header->mapped || keep >= header->size) return pointer;
    munmap((unsigned char *)header + keep, header->size - keep);
    header->size = keep;
    return pointer;
}
