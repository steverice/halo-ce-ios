#ifndef ASTC_REWRITE_H
#define ASTC_REWRITE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum astc_dxt_format { ASTC_DXT1, ASTC_DXT3, ASTC_DXT5 };

/* Source and output contain complete 4x4 blocks, including a mip's padded edge.
   Preflight every face and mip before choosing the GPU format for a texture. */
bool astc_rewrite_block(enum astc_dxt_format format, const uint8_t *source, uint8_t output[16]);
bool astc_rewrite_level_supported(enum astc_dxt_format format, const void *source, unsigned width, unsigned height);
/* A rejected level leaves output untouched. Output needs ceil(w/4)*ceil(h/4)*16 bytes. */
bool astc_rewrite_level(enum astc_dxt_format format, const void *source, unsigned width, unsigned height, void *output);

#ifdef __cplusplus
}
#endif
#endif
