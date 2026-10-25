/* Apache-2.0. Guest imports keep native store structs and pointers in the host. */
#include "astc_store.h"
#include <string.h>

int host_astc_texture_key(const void *source, unsigned int format,
    unsigned int width, unsigned int height, unsigned int levels, unsigned int faces,
    unsigned int source_face_bytes, void *key)
{
    struct astc_store_span spans[6 * 13];
    size_t count = 0, face_size = 0;
    if (!source || !key || format > 2 || !width || !height || width > 4096 || height > 4096 ||
        !levels || levels > 13 || (faces != 1 && faces != 6)) return 0;
    for (unsigned level = 0; level < levels; level++) {
        unsigned w = width >> level, h = height >> level;
        if (!w) w = 1;
        if (!h) h = 1;
        face_size += ((w + 3) / 4) * ((h + 3) / 4) * (format ? 16 : 8);
    }
    if (source_face_bytes < face_size) return 0;
    for (unsigned face = 0; face < faces; face++) {
        size_t offset = (size_t)face * source_face_bytes;
        for (unsigned level = 0; level < levels; level++) {
            unsigned w = width >> level, h = height >> level;
            if (!w) w = 1;
            if (!h) h = 1;
            size_t size = ((w + 3) / 4) * ((h + 3) / 4) * (format ? 16 : 8);
            spans[count++] = (struct astc_store_span){(const unsigned char *)source + offset, size};
            offset += size;
        }
    }
    astc_store_key_compute(key, format, width, height, levels, faces, spans, count);
    return 1;
}

int host_astc_texture_read(const void *key, void *bytes, unsigned int size)
{
    struct astc_store_record record = {0};
    int found = 0;
    if (key && bytes && astc_store_lookup(key, &record)) {
        if (size == record.size) {
            memcpy(bytes, record.bytes, size);
            found = 1;
        }
        astc_store_release(&record);
    }
    return found;
}

unsigned long long host_astc_texture_generation(void) { return astc_store_generation(); }
