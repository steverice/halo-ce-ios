/* Apache-2.0. Deterministic sampler metadata checks; does not submit GPU work. */
#define HALO_PERF_REPORTS 1
static int test_eligible = 1, test_collecting = 1;
int host_perf_reports_enabled(void) { return test_eligible; }
int host_perf_reports_collecting(void) { return test_eligible && test_collecting; }
#include "../host/host_perf_metal.m"
#include <assert.h>
static uint64_t test_frame = 1;
uint64_t host_perf_hooks_frame_id(void) { return test_frame; }
int main(void) {
    @autoreleasepool {
        NSObject *commands[BUFFER_COUNT + 1];
        for (unsigned i = 0; i <= BUFFER_COUNT; ++i) commands[i] = [NSObject new];
        assert(host_perf_metal_prepare((id)commands[0], nil, 0) == 0);
        mode = 1; /* Metadata needs no GPU capability or counter buffer. */
        test_eligible = 0;
        host_perf_metal_initialize(nil);
        assert(mode == 1 && counter_device == nil); /* disabled initialization is inert */
        assert(!host_perf_metal_prepare((id)commands[0], nil, 0));
        test_eligible = 1;
        test_collecting = 0; /* eligible tester build, suppressed automated run */
        assert(!host_perf_metal_prepare((id)commands[0], nil, 0));
        assert(!last_frame && !buffers[0].count && !atomic_load(&buffers[0].commands));
        assert(!host_perf_metal_dropped());
        test_collecting = 1;
        for (unsigned i = 0; i < INTERVAL_COUNT; ++i)
            assert(host_perf_metal_prepare((id)commands[0], nil, 0));
        assert(!host_perf_metal_prepare((id)commands[0], nil, 0));
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
        assert(host_perf_metal_prepare((id)commands[1], nil, 1));
        add(1, 1, 999, 1); /* late callback cannot enrich the reused generation */
        frame = (game_perf_frame){.frame_id = test_frame};
        host_perf_metal_join(&frame);
        assert(!frame.detail.available);
        add(test_frame, 1, 25, 1);
        add(test_frame, 1, 15, 1);
        host_perf_metal_join(&frame);
        assert(frame.detail.gpu_pass_us[1] == 40 && frame.detail.available == (1u << 9));
        for (unsigned i = 2; i < BUFFER_COUNT; ++i)
            assert(host_perf_metal_prepare((id)commands[i], nil, 2));
        assert(!host_perf_metal_prepare((id)commands[BUFFER_COUNT], nil, 2));
        assert(host_perf_metal_dropped() == 2);
        assert(sizeof(buffers) + sizeof(sums) + BUFFER_COUNT * INTERVAL_COUNT * 4 * sizeof(MTLCounterResultTimestamp) < 1024 * 1024);
    }
    return 0;
}
