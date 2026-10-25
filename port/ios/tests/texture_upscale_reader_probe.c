/* Reader/GPU boundary for the generator's real encoded record round trip. */
#include "texture_override_cache.h"
#include "texture_record.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct gpu_capabilities device_capabilities = { .max_texture_size = 8192 };
static const uint8_t *expected_record;
static struct texture_record_layout expected_layout;
static unsigned uploaded_levels, live_textures;
static uint64_t uploaded_bytes;
void platform_log(const char *format, ...) { (void)format; }
gpu_texture gpu_texture_create(const struct gpu_texture_description *description)
{
    assert(expected_record && description->width == expected_layout.width && description->height == expected_layout.height);
    assert(description->levels == expected_layout.levels);
    assert(description->format == (expected_layout.format == TEXTURE_RECORD_BC7 ? GPU_FORMAT_BC7 : GPU_FORMAT_ASTC4X4));
    ++live_textures;
    return 1;
}
void gpu_texture_destroy(gpu_texture texture)
{
    assert(texture && live_textures);
    --live_textures;
}
void gpu_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
    assert(texture && !face && level == uploaded_levels && size == expected_layout.level_size[level]);
    assert(!memcmp(data, expected_record + expected_layout.level_offset[level], size));
    ++uploaded_levels;
    uploaded_bytes += size;
}

void texture_upscale_reader_probe(const char *directory, uint32_t format, const uint8_t *record, size_t size)
{
    assert(!texture_record_check(record, size, size, 0x71, 4, 4, format, &expected_layout));
    assert(!texture_record_check_payload(&expected_layout, record + expected_layout.header_size));
    expected_record = record; uploaded_levels = 0; uploaded_bytes = 0;
    texture_override_cache_start(1 << 20);
    assert(texture_override_cache_records(directory, format, "") == 1);
    texture_override_cache_map_loaded(1);
    struct texture_override_reference reference = { 0 };
    unsigned long levels = 0;
    int fallback = -1;
    assert(texture_override_cache_find(&reference, 0x71, 4, 4, &levels, &fallback));
    assert(levels == expected_layout.levels && uploaded_levels == levels && !fallback && live_textures == 1);
    assert(uploaded_bytes == expected_layout.payload_size && texture_override_cache()->bytes == uploaded_bytes);
    texture_override_cache_map_loaded(0);
    assert(!live_textures);
    /* Corruption must be refused before any GPU allocation or block upload. */
    char path[512];
    snprintf(path, sizeof(path), "%s/0000000000000071-4x4.bin", directory);
    FILE *file = fopen(path, "r+b"); assert(file);
    assert(!fseek(file, (long)(size - 1), SEEK_SET));
    assert(fputc(record[size - 1] ^ 1, file) != EOF); assert(!fclose(file));
    assert(texture_override_cache_records(directory, format, "") == 1);
    texture_override_cache_map_loaded(1);
    memset(&reference, 0, sizeof(reference)); levels = 0; uploaded_levels = 0;
    assert(!texture_override_cache_find(&reference, 0x71, 4, 4, &levels, &fallback));
    assert(!live_textures && !uploaded_levels);
    texture_override_cache_map_loaded(0);
    assert(!texture_override_cache_records("", 0, ""));
    expected_record = NULL;
}
