/* Apache-2.0. Samples describe attachment classes, not inferred game work.
 * Semantic S/U/H uses the union of explicitly tagged encoder intervals.
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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <os/lock.h>
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
    unsigned char categories[INTERVAL_COUNT], groups[INTERVAL_COUNT];
} buffers[BUFFER_COUNT];
static _Atomic uint64_t sums[FRAME_COUNT][CATEGORY_COUNT], dropped;
#define GROUP_INTERVALS 256
static os_unfair_lock group_lock = OS_UNFAIR_LOCK_INIT;
static struct {
    uint64_t frame, presented, interval, budget;
    unsigned count, invalid, late;
    /* MetalFX owns its encoders, so counters cannot bracket it; with timing
     * it runs alone in a command buffer whose GPU span is the upscale. */
    unsigned upscale_expected, upscale_seen;
    uint64_t upscale_us;
    unsigned span_count;
    struct { double start, end; unsigned group; } spans[16];
    double scale;
    struct { uint64_t start, end; unsigned group; } ranges[GROUP_INTERVALS];
} grouped[FRAME_COUNT];
static uint64_t last_frame;
static uint64_t generation(uint64_t frame);
static void frame_prepare(uint64_t frame) {
    if (last_frame == frame) return;
    last_frame = frame;
    for (unsigned c = 0; c < CATEGORY_COUNT; ++c) atomic_store(&sums[frame % FRAME_COUNT][c], generation(frame));
    /* Semantic groups serve only the shadow planner; reports keep categories. */
    if (!host_perf_hooks_dynamic_enabled()) return;
    os_unfair_lock_lock(&group_lock);
    memset(&grouped[frame % FRAME_COUNT], 0, sizeof(grouped[0]));
    grouped[frame % FRAME_COUNT].frame = frame;
    os_unfair_lock_unlock(&group_lock);
}

static int mode; /* 0 unavailable, 1 draw boundary, 2 stage boundary */
static id<MTLDevice> counter_device, candidate_device;
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
/* a Metal device is in use: command-buffer GPU spans exist, the timing fallback's input */
static int metal_seen;
void host_perf_metal_initialize(id<MTLDevice> device) {
    /* Called once before play; never replace buffers while callbacks can run. */
    candidate_device = device;
    if (device) metal_seen = 1;
    if (counter_device || !device || !(host_perf_reports_enabled() || host_perf_hooks_dynamic_enabled())) return;
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
uint32_t host_perf_metal_prepare(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor *pass, unsigned category, unsigned group) {
    if (!(host_perf_reports_collecting() || host_perf_hooks_dynamic_enabled()) || category >= CATEGORY_COUNT || group > HOST_PERF_HUD) return 0;
    if (!mode) host_perf_metal_initialize(candidate_device);
    if (!mode) return 0;
    uint64_t frame = host_perf_hooks_frame_id();
    if (!frame) return 0;
    frame_prepare(frame);
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
        host_perf_metal_unavailable(group);
        return 0;
    }
    unsigned interval = buffers[slot].count++;
    buffers[slot].categories[interval] = category;
    buffers[slot].groups[interval] = group;
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
int host_perf_metal_needs_split(void) {
    /* Only shadow mode splits encoders; report builds keep the shared encoder. */
    return mode == 2 && host_perf_hooks_dynamic_enabled();
}
int host_perf_metal_timing_supported(void) {
    return host_perf_metal_stage_supported() || metal_seen;
}
void host_perf_metal_frame_touch(uint64_t frame) {
    if (frame && host_perf_hooks_dynamic_enabled()) frame_prepare(frame);
}
int host_perf_metal_stage_supported(void) {
    return counter_device && mode && [counter_device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary];
}
void host_perf_metal_unavailable(unsigned group) {
    if (!host_perf_hooks_dynamic_enabled()) return;
    uint64_t frame = host_perf_hooks_frame_id();
    if (!frame || group > HOST_PERF_HUD) return;
    frame_prepare(frame);
    os_unfair_lock_lock(&group_lock);
    grouped[frame % FRAME_COUNT].invalid |= 1u << group;
    os_unfair_lock_unlock(&group_lock);
}
uint32_t host_perf_metal_switch(uint32_t token, id<MTLRenderCommandEncoder> encoder, unsigned group) {
    if (!token) return 0;
    unsigned slot = (token - 1) / INTERVAL_COUNT, interval = (token - 1) % INTERVAL_COUNT;
    if (mode != 1) {
        host_perf_metal_unavailable(buffers[slot].groups[interval]);
        host_perf_metal_unavailable(group);
        return token;
    }
    host_perf_metal_end(token, encoder);
    /* Draw sampling does not use a render-pass attachment. */
    uint32_t next = host_perf_metal_prepare((__bridge id<MTLCommandBuffer>)(void *)atomic_load(&buffers[slot].commands),
        nil, buffers[slot].categories[interval], group);
    host_perf_metal_begin(next, encoder);
    return next;
}
void host_perf_metal_upscale_expected(void) {
    if (!host_perf_hooks_dynamic_enabled()) return;
    uint64_t frame = host_perf_hooks_frame_id();
    if (!frame) return;
    frame_prepare(frame);
    os_unfair_lock_lock(&group_lock);
    grouped[frame % FRAME_COUNT].upscale_expected++;
    os_unfair_lock_unlock(&group_lock);
}
void host_perf_metal_upscale_time(uint64_t frame, uint64_t duration_us, int valid) {
    if (!frame) return;
    os_unfair_lock_lock(&group_lock);
    typeof(grouped[0]) *entry = &grouped[frame % FRAME_COUNT];
    if (entry->frame == frame) {
        if (valid) { entry->upscale_seen++; entry->upscale_us += duration_us; }
        else entry->invalid |= 1u << HOST_PERF_UPSCALE;
    }
    os_unfair_lock_unlock(&group_lock);
}
void host_perf_metal_command_span(uint64_t frame, unsigned group, double start, double end) {
    if (!frame || end < start) return;
    os_unfair_lock_lock(&group_lock);
    typeof(grouped[0]) *entry = &grouped[frame % FRAME_COUNT];
    if (entry->frame == frame && entry->span_count < 16) {
        unsigned n = entry->span_count++;
        entry->spans[n].start = start; entry->spans[n].end = end; entry->spans[n].group = group;
    }
    os_unfair_lock_unlock(&group_lock);
}
/* the union of the frame's command-buffer spans in a part (scene, or the rest), microseconds */
static double span_union(const typeof(grouped[0]) *entry, int scene) {
    double starts[16], ends[16], total = 0.0, reach = -1.0;
    unsigned n = 0;
    for (unsigned i = 0; i < entry->span_count; i++)
        if ((entry->spans[i].group == HOST_PERF_SCENE) == scene) {
            unsigned j = n++;
            while (j && starts[j - 1] > entry->spans[i].start) { starts[j] = starts[j - 1]; ends[j] = ends[j - 1]; j--; }
            starts[j] = entry->spans[i].start; ends[j] = entry->spans[i].end;
        }
    for (unsigned i = 0; i < n; i++) {
        double from = starts[i] > reach ? starts[i] : reach;
        if (ends[i] > from) total += ends[i] - from;
        if (ends[i] > reach) reach = ends[i];
    }
    return total * 1e6;
}
static int compare_doubles(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
/* every 600 frames with both: how far the command-buffer union is from stage timing, all frames and late ones */
static void timing_check(double stage_scene, double stage_rest, double buffer_scene, double buffer_rest, int late) {
    enum { N = 600 };
    static double scene_error[N], rest_error[N], late_error[N];
    static unsigned count, late_count;
    static unsigned long logged;
    if (stage_scene <= 0.0) return;
    scene_error[count] = (buffer_scene - stage_scene) / stage_scene;
    rest_error[count] = buffer_rest - stage_rest;
    if (late && late_count < N) late_error[late_count++] = (buffer_scene - stage_scene) / stage_scene;
    if (++count < N) return;
    qsort(scene_error, N, sizeof(double), compare_doubles); qsort(rest_error, N, sizeof(double), compare_doubles);
    if (late_count) qsort(late_error, late_count, sizeof(double), compare_doubles);
    fprintf(stderr, "DRS timing check %lu: scene buffers vs stage S: median %+.1f%% p5 %+.1f%% p95 %+.1f%%; "
        "rest buffers minus U+H: median %+.0f us p95 %+.0f us; late frames %u, their scene median %+.1f%% p95 %+.1f%%\n",
        ++logged, scene_error[N / 2] * 100, scene_error[N / 20] * 100, scene_error[N * 19 / 20] * 100,
        rest_error[N / 2], rest_error[N * 19 / 20], late_count,
        late_count ? late_error[late_count / 2] * 100 : 0.0, late_count ? late_error[late_count * 19 / 20] * 100 : 0.0);
    count = late_count = 0;
}
void host_perf_metal_presented(uint64_t frame, uint64_t time_us, uint64_t budget_us) {
    if (!frame) return;
    os_unfair_lock_lock(&group_lock);
    typeof(grouped[0]) *entry = &grouped[frame % FRAME_COUNT];
    static unsigned presentation_diagnostics;
    if (presentation_diagnostics++ < 4)
        fprintf(stderr, "DRS presentation diagnostic: callback_frame=%llu slot_frame=%llu presented_us=%llu\n",
            (unsigned long long)frame, (unsigned long long)entry->frame, (unsigned long long)time_us);
    if (!time_us) { os_unfair_lock_unlock(&group_lock); return; }
    if (entry->frame == frame) {
        entry->presented = time_us;
        entry->budget = budget_us;
        /* Metal callbacks may arrive out of order. Join by frame ID, and
         * repair the next shown frame if its callback arrived first. */
        typeof(grouped[0]) *previous = NULL, *next = NULL;
        for (unsigned i = 0; i < FRAME_COUNT; ++i) {
            if (!grouped[i].presented) continue;
            if (grouped[i].frame < frame && (!previous || grouped[i].frame > previous->frame)) previous = &grouped[i];
            if (grouped[i].frame > frame && (!next || grouped[i].frame < next->frame)) next = &grouped[i];
        }
        if (previous && time_us > previous->presented) {
            entry->interval = time_us - previous->presented;
            entry->late = budget_us && entry->interval > budget_us * 3 / 2;
        }
        if (next && next->presented > time_us) {
            next->interval = next->presented - time_us;
            next->late = next->budget && next->interval > next->budget * 3 / 2;
        }
    }
    os_unfair_lock_unlock(&group_lock);
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
                if (!host_perf_hooks_dynamic_enabled()) continue;
                os_unfair_lock_lock(&group_lock);
                typeof(grouped[0]) *frame = &grouped[buffers[slot].frame % FRAME_COUNT];
                if (frame->frame == buffers[slot].frame) {
                    if (!valid || frame->count == GROUP_INTERVALS) frame->invalid |= 1u << buffers[slot].groups[i];
                    else {
                        /* Absolute GPU ticks share one device clock across command buffers. */
                        frame->scale = scale;
                        unsigned n = frame->count++;
                        frame->ranges[n].start = start;
                        frame->ranges[n].end = end;
                        frame->ranges[n].group = buffers[slot].groups[i];
                    }
                }
                os_unfair_lock_unlock(&group_lock);
            }
        }
        atomic_store(&buffers[slot].commands, 0);
        break;
    }
}
/* The timing fallback's U+H: the least of the last 32 frames' rest spans. The present buffer holds the HUD's
 * passes and the drawable copy, so its span includes a wait for the drawable whenever the display is behind; on a
 * CPU-bound A9X iPad even the median read 3-16 ms. The HUD's own work is about the same every frame, so the
 * least recent span is its cost; a wait only ever adds. An underestimate errs toward not dropping. */
static double rest_estimate(double rest) {
    static double ring[32];
    static unsigned count, next;
    double least;
    ring[next] = rest; next = (next + 1) % 32; if (count < 32) count++;
    least = ring[0];
    for (unsigned i = 1; i < count; i++) if (ring[i] < least) least = ring[i];
    return least;
}
static void dynamic_join(game_perf_frame *frame) {
    if (host_perf_metal_timing_supported()) frame->drs.available |= 8;
    os_unfair_lock_lock(&group_lock);
    typeof(grouped[0]) *entry = &grouped[frame->frame_id % FRAME_COUNT];
    if (entry->frame == frame->frame_id) {
        frame->drs.presented_us = entry->presented;
        frame->drs.presentation_us = entry->interval;
        frame->drs.late = entry->late;
        if (entry->interval) frame->drs.available |= 2;
        /* Sort stage envelopes, then charge overlapping ticks once to the
         * earliest-starting pass. This is a union, never a sum of envelopes. */
        for (unsigned i = 1; i < entry->count; ++i) {
            typeof(entry->ranges[0]) range = entry->ranges[i];
            unsigned j = i;
            while (j && entry->ranges[j - 1].start > range.start) {
                entry->ranges[j] = entry->ranges[j - 1]; --j;
            }
            entry->ranges[j] = range;
        }
        uint64_t end = 0, totals[3] = {0};
        for (unsigned i = 0; i < entry->count; ++i) {
            uint64_t start = entry->ranges[i].start > end ? entry->ranges[i].start : end;
            if (entry->ranges[i].end > start)
                totals[entry->ranges[i].group] += entry->ranges[i].end - start;
            if (entry->ranges[i].end > end) end = entry->ranges[i].end;
        }
        if (!entry->invalid && entry->count && host_perf_metal_stage_supported() && !(frame->flags & GAME_PERF_GPU_UNAVAILABLE) &&
            entry->upscale_seen == entry->upscale_expected) {
            frame->drs.scene_us = (uint64_t)(totals[0] * entry->scale);
            frame->drs.upscale_us = (uint64_t)(totals[1] * entry->scale) + entry->upscale_us;
            frame->drs.hud_us = (uint64_t)(totals[2] * entry->scale);
            frame->drs.available |= 1;
            if (entry->span_count)
                timing_check((double)frame->drs.scene_us, (double)(frame->drs.upscale_us + frame->drs.hud_us),
                    span_union(entry, 1), span_union(entry, 0), (int)entry->late);
        }
        /* No stage-boundary sampling: the scene's command buffers' GPU spans are S (on a device with both they
         * matched stage timing in every window), and the rest's least recent span is U+H; marked coarse (16) */
        else if (!host_perf_metal_stage_supported() && entry->span_count && !(entry->invalid & (1u << HOST_PERF_SCENE)) &&
            !(frame->flags & GAME_PERF_GPU_UNAVAILABLE)) {
            double scene = span_union(entry, 1), rest = span_union(entry, 0);
            if (scene > 0.0) {
                frame->drs.scene_us = (uint64_t)scene;
                frame->drs.upscale_us = 0;
                frame->drs.hud_us = (uint64_t)rest_estimate(rest);
                frame->drs.available |= 1 | 16;
            }
        }
    }
    os_unfair_lock_unlock(&group_lock);
}
void host_perf_metal_join(game_perf_frame *frame) {
    if (host_perf_hooks_dynamic_enabled()) dynamic_join(frame);
    if (frame->flags & GAME_PERF_GPU_UNAVAILABLE) return;
    for (unsigned c = 0; c < CATEGORY_COUNT; ++c) {
        uint64_t value = atomic_load(&sums[frame->frame_id % FRAME_COUNT][c]);
        if ((value & GENERATION_MASK) != generation(frame->frame_id) || !(value & SEEN) || (value & INVALID)) continue;
        frame->detail.gpu_pass_us[c] = (uint32_t)(value & VALUE_MASK);
        frame->detail.available |= 1u << (8 + c);
    }
}
uint64_t host_perf_metal_dropped(void) { return atomic_load(&dropped); }
