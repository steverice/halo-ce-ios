/* Apache-2.0. Bounded asynchronous Metal timestamp collection. */
#pragma once
#include <stdint.h>
struct game_perf_frame;
void host_perf_metal_join(struct game_perf_frame *frame);
uint64_t host_perf_metal_dropped(void);
#ifdef __OBJC__
#import <Metal/Metal.h>
void host_perf_metal_initialize(id<MTLDevice> device);
/* Categories describe attachment classes: 0 color+depth, 1 color-only, 2 depth-only.
 * A token covers one encoder. Zero means unsupported or capacity exhausted. */
uint32_t host_perf_metal_prepare(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor *pass, unsigned category);
void host_perf_metal_begin(uint32_t token, id<MTLRenderCommandEncoder> encoder);
void host_perf_metal_end(uint32_t token, id<MTLRenderCommandEncoder> encoder);
/* Call from the existing handler, before publishing command-buffer completion. */
void host_perf_metal_complete(id<MTLCommandBuffer> commands);
#endif
