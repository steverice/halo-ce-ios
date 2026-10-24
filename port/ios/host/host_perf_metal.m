/* Apache-2.0. Samples describe attachment classes, not inferred game work.
 * Draw-boundary durations or vertex/fragment stage envelopes are summed per
 * category. Envelopes may overlap; these values must not be summed as GPU busy
 * time. Only completed command buffers from the attributed CPU frame join.
 * See Apple's "Converting GPU timestamps into CPU time" for clock calibration.
 */
#import "host_perf_metal.h"
#include "host_perf_hooks.h"
#include "host_perf_reports.h"
#include "game_perf.h"
#include <stdatomic.h>
#include <math.h>
#if HALO_PERF_REPORTS
#define BUFFER_COUNT 32
#define INTERVAL_COUNT 64
#define FRAME_COUNT 128
#define CATEGORY_COUNT 3
#define VALUE_MASK UINT64_C(0x3fffffff)
#define SEEN UINT64_C(0x40000000)
#define INVALID UINT64_C(0x80000000)
#define GENERATION_MASK UINT64_C(0xffffffff00000000)
/* 65,536 bytes of requested timestamp payload, plus bounded metadata. Metal's
 * counter buffer's internal representation is opaque and has no size API. */
static struct {
    id<MTLCounterSampleBuffer> buffer;
    _Atomic uintptr_t commands;
    uint64_t frame;
    unsigned count;
    unsigned char categories[INTERVAL_COUNT];
} buffers[BUFFER_COUNT];
static _Atomic uint64_t sums[FRAME_COUNT][CATEGORY_COUNT], dropped;
static uint64_t last_frame;
static int mode; /* 0 unavailable, 1 draw boundary, 2 stage boundary */
static id<MTLDevice> counter_device;
static MTLTimestamp reference_cpu, reference_gpu;
static uint64_t generation(uint64_t frame) { return (uint64_t)(uint32_t)frame << 32; }
static void add(uint64_t frame, unsigned category, uint64_t duration, int valid) {
    _Atomic uint64_t *value = &sums[frame % FRAME_COUNT][category];
    uint64_t tag = generation(frame), seen = atomic_load(value);
    while ((seen & GENERATION_MASK) == tag) {
        uint64_t total = (seen & VALUE_MASK) + duration;
        uint64_t next = tag | SEEN | (seen & INVALID) | (!valid || total > VALUE_MASK ? INVALID : 0) |
            (total > VALUE_MASK ? VALUE_MASK : total);
        if (atomic_compare_exchange_weak(value, &seen, next)) break;
    }
}
void host_perf_metal_initialize(id<MTLDevice> device) {
    /* Called once before play; never replace buffers while callbacks can run. */
    if (!host_perf_reports_enabled() || counter_device) return;
    counter_device = device;
    if ([device supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary]) mode = 1;
    else if ([device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) mode = 2;
    if (!mode) return;
    id<MTLCounterSet> timestamps = nil;
    for (id<MTLCounterSet> set in device.counterSets) {
        if (![set.name isEqualToString:MTLCommonCounterSetTimestamp]) continue;
        for (id<MTLCounter> counter in set.counters)
            if ([counter.name isEqualToString:MTLCommonCounterTimestamp]) timestamps = set;
    }
    if (!timestamps) { mode = 0; return; }
    MTLCounterSampleBufferDescriptor *descriptor = [MTLCounterSampleBufferDescriptor new];
    descriptor.counterSet = timestamps;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.sampleCount = INTERVAL_COUNT * 4;
    for (unsigned i = 0; i < BUFFER_COUNT; ++i) {
        buffers[i].buffer = [device newCounterSampleBufferWithDescriptor:descriptor error:nil];
        if (!buffers[i].buffer) {
            mode = 0;
            for (unsigned j = 0; j < BUFFER_COUNT; ++j) buffers[j].buffer = nil;
            return;
        }
    }
    [device sampleTimestamps:&reference_cpu gpuTimestamp:&reference_gpu];
}
uint32_t host_perf_metal_prepare(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor *pass, unsigned category) {
    if (!host_perf_reports_collecting() || !mode || category >= CATEGORY_COUNT) return 0;
    uint64_t frame = host_perf_hooks_frame_id();
    if (!frame) return 0;
    if (last_frame != frame) {
        last_frame = frame;
        for (unsigned c = 0; c < CATEGORY_COUNT; ++c) atomic_store(&sums[frame % FRAME_COUNT][c], generation(frame));
    }
    uintptr_t key = (uintptr_t)(__bridge void *)commands;
    unsigned slot;
    for (slot = 0; slot < BUFFER_COUNT; ++slot) if (atomic_load(&buffers[slot].commands) == key) break;
    if (slot == BUFFER_COUNT) {
        for (slot = 0; slot < BUFFER_COUNT; ++slot) {
            uintptr_t empty = 0;
            if (atomic_compare_exchange_strong(&buffers[slot].commands, &empty, key)) {
                buffers[slot].frame = frame;
                buffers[slot].count = 0;
                break;
            }
        }
    }
    if (slot == BUFFER_COUNT || buffers[slot].count == INTERVAL_COUNT || buffers[slot].frame != frame) {
        atomic_fetch_add(&dropped, 1);
        add(frame, category, 0, 0);
        return 0;
    }
    unsigned interval = buffers[slot].count++;
    buffers[slot].categories[interval] = category;
    if (mode == 2) {
        MTLRenderPassSampleBufferAttachmentDescriptor *attachment = pass.sampleBufferAttachments[0];
        attachment.sampleBuffer = buffers[slot].buffer;
        attachment.startOfVertexSampleIndex = interval * 4;
        attachment.endOfVertexSampleIndex = interval * 4 + 1;
        attachment.startOfFragmentSampleIndex = interval * 4 + 2;
        attachment.endOfFragmentSampleIndex = interval * 4 + 3;
    }
    return 1 + slot * INTERVAL_COUNT + interval;
}
void host_perf_metal_begin(uint32_t token, id<MTLRenderCommandEncoder> encoder) {
    if (!token) return;
    unsigned slot = (token - 1) / INTERVAL_COUNT, interval = (token - 1) % INTERVAL_COUNT;
    if (!encoder) { add(buffers[slot].frame, buffers[slot].categories[interval], 0, 0); return; }
    if (mode != 1) return;
    [encoder sampleCountersInBuffer:buffers[slot].buffer atSampleIndex:interval * 4 withBarrier:NO];
}
void host_perf_metal_end(uint32_t token, id<MTLRenderCommandEncoder> encoder) {
    if (!token || mode != 1) return;
    unsigned slot = (token - 1) / INTERVAL_COUNT, interval = (token - 1) % INTERVAL_COUNT;
    [encoder sampleCountersInBuffer:buffers[slot].buffer atSampleIndex:interval * 4 + 1 withBarrier:NO];
}
void host_perf_metal_complete(id<MTLCommandBuffer> commands) {
    if (!mode) return;
    uintptr_t key = (uintptr_t)(__bridge void *)commands;
    for (unsigned slot = 0; slot < BUFFER_COUNT; ++slot) {
        if (atomic_load(&buffers[slot].commands) != key) continue;
        @autoreleasepool {
            NSData *data = commands.status == MTLCommandBufferStatusCompleted ?
                [buffers[slot].buffer resolveCounterRange:NSMakeRange(0, buffers[slot].count * 4)] : nil;
            MTLTimestamp cpu = 0, gpu = 0;
            [counter_device sampleTimestamps:&cpu gpuTimestamp:&gpu];
            /* Apple documents the correlated CPU reference in nanoseconds. */
            double scale = cpu > reference_cpu && gpu > reference_gpu ?
                (double)(cpu - reference_cpu) / (double)(gpu - reference_gpu) / 1000.0 : 0.0;
            const MTLCounterResultTimestamp *samples = data.bytes;
            for (unsigned i = 0; i < buffers[slot].count; ++i) {
                uint64_t start = UINT64_MAX, end = 0;
                int valid = scale > 0.0 && data.length >= (i + 1) * 4 * sizeof(*samples);
                unsigned count = mode == 2 ? 4 : 2;
                for (unsigned j = 0; valid && j < count; ++j) {
                    uint64_t time = samples[i * 4 + j].timestamp;
                    if (!time || time == MTLCounterErrorValue ||
                        ((j & 1) && time < samples[i * 4 + j - 1].timestamp)) { valid = 0; break; }
                    if (!(j & 1) && time < start) start = time;
                    if ((j & 1) && time > end) end = time;
                }
                double duration = valid && end >= start ? (double)(end - start) * scale : -1.0;
                valid = isfinite(duration) && duration >= 0 && duration <= VALUE_MASK;
                add(buffers[slot].frame, buffers[slot].categories[i], valid ? (uint64_t)duration : 0, valid);
            }
        }
        atomic_store(&buffers[slot].commands, 0);
        break;
    }
}
void host_perf_metal_join(game_perf_frame *frame) {
    if (frame->flags & GAME_PERF_GPU_UNAVAILABLE) return;
    for (unsigned c = 0; c < CATEGORY_COUNT; ++c) {
        uint64_t value = atomic_load(&sums[frame->frame_id % FRAME_COUNT][c]);
        if ((value & GENERATION_MASK) != generation(frame->frame_id) || !(value & SEEN) || (value & INVALID)) continue;
        frame->detail.gpu_pass_us[c] = (uint32_t)(value & VALUE_MASK);
        frame->detail.available |= 1u << (8 + c);
    }
}
uint64_t host_perf_metal_dropped(void) { return atomic_load(&dropped); }
#else
void host_perf_metal_initialize(id<MTLDevice> device) { (void)device; }
uint32_t host_perf_metal_prepare(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor *pass, unsigned category) { (void)commands; (void)pass; (void)category; return 0; }
void host_perf_metal_begin(uint32_t token, id<MTLRenderCommandEncoder> encoder) { (void)token; (void)encoder; }
void host_perf_metal_end(uint32_t token, id<MTLRenderCommandEncoder> encoder) { (void)token; (void)encoder; }
void host_perf_metal_complete(id<MTLCommandBuffer> commands) { (void)commands; }
void host_perf_metal_join(game_perf_frame *frame) { (void)frame; }
uint64_t host_perf_metal_dropped(void) { return 0; }
#endif
