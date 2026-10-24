/* Apache-2.0. Deterministic clocks pin phase subtraction and failed attempts. */
#include "host_perf_hooks.h"
#include "host_perf_reports.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
void host_config_string(const char *name, const char *fallback, char *out, size_t size) { (void)name; snprintf(out,size,"%s",fallback); }
void host_perf_metal_join(game_perf_frame *frame) { (void)frame; }
static game_perf_frame recorded;
static game_perf_event event;
void host_perf_reports_frame(const game_perf_frame *frame) { recorded = *frame; }
void host_perf_reports_event(const game_perf_event *value) { event = *value; }
int main(void) {
    host_perf_hooks_phase_at(7, 0, 1000000);
    host_perf_hooks_phase_at(0, 0, 1001000);
    host_perf_hooks_phase_at(5, 0, 1002000);
    host_perf_hooks_phase_at(6, 0, 1005000);
    host_perf_hooks_phase_at(1, 2, 1009000);
    host_perf_hooks_phase_at(2, 0, 1010000);
    host_perf_hooks_phase_at(3, 0, 1014000);
    uint64_t gpu_token = host_perf_hooks_gpu_reserve();
    host_perf_hooks_gpu_complete(gpu_token, 4000, 1);
    host_perf_hooks_budget(16666);
    host_perf_hooks_present_begin_at(1015000);
    host_perf_hooks_present_failed();
    host_perf_hooks_present_end_at(1017000);
    host_perf_hooks_gpu_join(&recorded);
    assert(recorded.gpu_us == 4000);
    recorded.gpu_us = GAME_PERF_UNAVAILABLE;
    assert(recorded.cpu_us == 15000 && recorded.simulation_us == 5000);
    assert(recorded.network_us == 3000 && recorded.render_us == 4000);
    assert(recorded.gpu_us == GAME_PERF_UNAVAILABLE && recorded.budget_us == 16666);
    assert(recorded.flags & GAME_PERF_PRESENT_FAILED);
    assert(recorded.tick_delta == 2);
    host_perf_hooks_phase_at(7, 0, 1020000);
    host_perf_hooks_present_begin_at(1021000);
    host_perf_hooks_cpu(4321);
    host_perf_hooks_present_end_at(1022000);
    host_perf_hooks_gpu_join(&recorded);
    assert(recorded.gpu_us == GAME_PERF_UNAVAILABLE);
    assert(recorded.interval_us == 20000 && recorded.cpu_us == 4321);
    assert(recorded.simulation_us == GAME_PERF_UNAVAILABLE);
    assert(recorded.network_us == 0 && !(recorded.flags & GAME_PERF_PRESENT_FAILED));
    host_perf_hooks_phase_at(8, GAME_PERF_EVENT_CHECKPOINT_LOAD, 1025000);
    assert(event.type == GAME_PERF_EVENT_CHECKPOINT_LOAD && event.elapsed_us == 1025000);
    return 0;
}
