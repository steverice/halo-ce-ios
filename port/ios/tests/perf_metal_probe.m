/* Apache-2.0. Deterministic sampler metadata checks; does not submit GPU work. */
#define HALO_PERF_REPORTS 1
static int test_eligible = 1, test_collecting = 1, test_shadow;
int host_perf_hooks_dynamic_enabled(void) { return test_shadow; }
int host_perf_reports_enabled(void) { return test_eligible; }
int host_perf_reports_collecting(void) { return test_eligible && test_collecting; }
#include "../host/host_perf_metal.m"
#include <assert.h>
static uint64_t test_frame = 1;
uint64_t host_perf_hooks_frame_id(void) { return test_frame; }
@interface TestCounterDevice : NSObject
@end
@implementation TestCounterDevice
- (BOOL)supportsCounterSampling:(MTLCounterSamplingPoint)point { (void)point; return YES; }
@end
int main(void) {
    @autoreleasepool {
        NSObject *commands[BUFFER_COUNT + 1];
        for (unsigned i = 0; i <= BUFFER_COUNT; ++i) commands[i] = [NSObject new];
        assert(host_perf_metal_prepare((id)commands[0], nil, 0, HOST_PERF_SCENE) == 0);
        mode = 1; /* Metadata needs no GPU capability or counter buffer. */
        test_eligible = 0;
        host_perf_metal_initialize(nil);
        assert(mode == 1 && counter_device == nil); /* disabled initialization is inert */
        assert(!host_perf_metal_prepare((id)commands[0], nil, 0, HOST_PERF_SCENE));
        test_eligible = 1;
        test_collecting = 0; /* eligible tester build, suppressed automated run */
        assert(!host_perf_metal_prepare((id)commands[0], nil, 0, HOST_PERF_SCENE));
        assert(!last_frame && !buffers[0].count && !atomic_load(&buffers[0].commands));
        assert(!host_perf_metal_dropped());
        mode = 2;
        assert(!host_perf_metal_needs_split()); /* suppressed reports and shadow off */
        test_shadow = 1;
        assert(host_perf_metal_needs_split()); /* shadow-only stage collection */
        test_shadow = 0; test_collecting = 1;
        assert(!host_perf_metal_needs_split()); /* reports alone keep the shared encoder */
        mode = 1;
        assert(!host_perf_metal_needs_split()); /* draw samples share the encoder */
        test_collecting = 1;
        for (unsigned i = 0; i < INTERVAL_COUNT; ++i)
            assert(host_perf_metal_prepare((id)commands[0], nil, 0, HOST_PERF_SCENE));
        assert(!host_perf_metal_prepare((id)commands[0], nil, 0, HOST_PERF_SCENE));
        assert(host_perf_metal_dropped() == 1);
        game_perf_frame frame = {.frame_id = 1};
        host_perf_metal_join(&frame);
        assert(!(frame.detail.available & (1u << 8))); /* partial category refused */
        add(1, 1, 0, 1); /* a measured submicrosecond interval can round to zero */
        host_perf_metal_join(&frame);
        assert(frame.detail.available & (1u << 9));
        frame.flags = GAME_PERF_GPU_UNAVAILABLE;
        frame.detail.available = 0;
        host_perf_metal_join(&frame);
        assert(!frame.detail.available); /* unresolved overall frame */
        test_frame += FRAME_COUNT;
        assert(host_perf_metal_prepare((id)commands[1], nil, 1, HOST_PERF_SCENE));
        add(1, 1, 999, 1); /* late callback cannot enrich the reused generation */
        frame = (game_perf_frame){.frame_id = test_frame};
        host_perf_metal_join(&frame);
        assert(!frame.detail.available);
        add(test_frame, 1, 25, 1);
        add(test_frame, 1, 15, 1);
        host_perf_metal_join(&frame);
        assert(frame.detail.gpu_pass_us[1] == 40 && frame.detail.available == (1u << 9));
        for (unsigned i = 2; i < BUFFER_COUNT; ++i)
            assert(host_perf_metal_prepare((id)commands[i], nil, 2, HOST_PERF_SCENE));
        assert(!host_perf_metal_prepare((id)commands[BUFFER_COUNT], nil, 2, HOST_PERF_SCENE));
        assert(host_perf_metal_dropped() == 2);
        /* Shadow off: reports touch no semantic-group state or lock. */
        test_frame = 400; frame_prepare(test_frame);
        host_perf_metal_unavailable(HOST_PERF_SCENE);
        assert(grouped[400 % FRAME_COUNT].frame != 400 && !grouped[400 % FRAME_COUNT].invalid);
        /* Presentation callbacks repair frame-ID attribution when reordered. */
        test_shadow = 1;
        test_frame = 500;
        frame_prepare(test_frame);
        host_perf_metal_presented(500, 1000000, 16667);
        test_frame = 501; frame_prepare(test_frame);
        test_frame = 502; frame_prepare(test_frame);
        host_perf_metal_presented(502, 1050000, 16667);
        host_perf_metal_presented(501, 1016667, 16667);
        frame = (game_perf_frame){.frame_id = 502};
        host_perf_metal_join(&frame);
        assert(frame.drs.presentation_us == 33333 && frame.drs.late && (frame.drs.available & 2));
        frame = (game_perf_frame){.frame_id = 501};
        host_perf_metal_join(&frame);
        assert(frame.drs.presentation_us == 16667 && !frame.drs.late);
        test_frame = 502 + FRAME_COUNT; frame_prepare(test_frame);
        host_perf_metal_presented(502, 9999999, 16667); /* stale callback */
        assert(!grouped[test_frame % FRAME_COUNT].presented);
        /* Stage-envelope union charges overlapping ticks exactly once. */
        counter_device = (id)[TestCounterDevice new];
        typeof(grouped[0]) *entry = &grouped[test_frame % FRAME_COUNT];
        entry->scale = 1.0; entry->count = 3;
        entry->ranges[0] = (typeof(entry->ranges[0])){ 200, 400, HOST_PERF_HUD };
        entry->ranges[1] = (typeof(entry->ranges[0])){ 100, 250, HOST_PERF_SCENE };
        entry->ranges[2] = (typeof(entry->ranges[0])){ 150, 300, HOST_PERF_UPSCALE };
        frame = (game_perf_frame){.frame_id = test_frame};
        host_perf_metal_join(&frame);
        assert(frame.drs.available & 1);
        assert(frame.drs.scene_us == 150 && frame.drs.upscale_us == 50 && frame.drs.hud_us == 100);
        entry->invalid = 1u << HOST_PERF_UPSCALE;
        frame = (game_perf_frame){.frame_id = test_frame};
        host_perf_metal_join(&frame);
        assert(!(frame.drs.available & 1));
        test_collecting = 0; test_shadow = 1;
        test_frame++; frame_prepare(test_frame);
        atomic_store(&buffers[1].commands, 0);
        assert(host_perf_metal_prepare((id)commands[BUFFER_COUNT], nil, 1, HOST_PERF_SCENE));
        /* No stage-boundary sampling: the scene's command-buffer spans (a union, overlap counted once) are S,
         * the least of the rest's last 32 spans is U+H, and the frame is marked coarse */
        counter_device = nil; mode = 0; metal_seen = 1;
        assert(host_perf_metal_timing_supported() && !host_perf_metal_stage_supported());
        for (unsigned f = 0; f < 3; ++f) {
            test_frame++;
            host_perf_metal_frame_touch(test_frame);
            host_perf_metal_command_span(test_frame, HOST_PERF_SCENE, 1.000, 1.004);
            host_perf_metal_command_span(test_frame, HOST_PERF_SCENE, 1.003, 1.006); /* overlaps by 1 ms */
            host_perf_metal_command_span(test_frame, HOST_PERF_HUD, 1.006, 1.007 + (f == 2 ? 0.003 : 0.0));
            frame = (game_perf_frame){.frame_id = test_frame};
            host_perf_metal_join(&frame);
            assert((frame.drs.available & (1 | 8 | 16)) == (1 | 8 | 16));
            assert(frame.drs.scene_us >= 5999 && frame.drs.scene_us <= 6001);
            /* (the third frame's 4 ms present span is a drawable wait; the estimate stays near 1 ms) */
            assert(frame.drs.upscale_us == 0 && frame.drs.hud_us >= 999 && frame.drs.hud_us <= 1001);
        }
        /* a refused scene (several views) gives no reading */
        test_frame++; host_perf_metal_frame_touch(test_frame);
        host_perf_metal_command_span(test_frame, HOST_PERF_SCENE, 2.0, 2.005);
        host_perf_metal_unavailable(HOST_PERF_SCENE);
        frame = (game_perf_frame){.frame_id = test_frame};
        host_perf_metal_join(&frame);
        assert(!(frame.drs.available & 1));
        assert(sizeof(buffers) + sizeof(sums) + sizeof(grouped) + BUFFER_COUNT * INTERVAL_COUNT * 4 * sizeof(MTLCounterResultTimestamp) < 1024 * 1024);
    }
    return 0;
}
