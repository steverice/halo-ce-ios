/* Apache-2.0. Bounded asynchronous Metal timestamp collection. */
#pragma once
#include <stdint.h>
struct game_perf_frame;
void host_perf_metal_join(struct game_perf_frame *frame);
uint64_t host_perf_metal_dropped(void);
int host_perf_metal_stage_supported(void);
/* Stage-only sampling needs an encoder boundary between semantic groups. */
int host_perf_metal_needs_split(void);
void host_perf_metal_presented(uint64_t frame, uint64_t time_us, uint64_t budget_us);
void host_perf_metal_unavailable(unsigned group);
/* MetalFX's upscale, timed as a command buffer of its own: expected while
 * encoding the current frame, then its GPU duration from the completion
 * handler. A frame whose expected durations have not all arrived has no U. */
void host_perf_metal_upscale_expected(void);
void host_perf_metal_upscale_time(uint64_t frame, uint64_t duration_us, int valid);
enum host_perf_metal_group { HOST_PERF_SCENE, HOST_PERF_UPSCALE, HOST_PERF_HUD };
#ifdef __OBJC__
#import <Metal/Metal.h>
void host_perf_metal_initialize(id<MTLDevice> device);
/* Categories describe attachment classes: 0 color+depth, 1 color-only, 2 depth-only.
 * A token covers one encoder. Zero means unsupported or capacity exhausted. */
uint32_t host_perf_metal_prepare(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor *pass, unsigned category, unsigned group);
/* Reclassify a shared encoder at a draw boundary without changing rendering.
 * Stage-only devices conservatively invalidate both groups in that encoder. */
uint32_t host_perf_metal_switch(uint32_t token, id<MTLRenderCommandEncoder> encoder, unsigned group);
void host_perf_metal_begin(uint32_t token, id<MTLRenderCommandEncoder> encoder);
void host_perf_metal_end(uint32_t token, id<MTLRenderCommandEncoder> encoder);
/* Call from the existing handler, before publishing command-buffer completion. */
void host_perf_metal_complete(id<MTLCommandBuffer> commands);
#endif
