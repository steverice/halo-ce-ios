#include "host_frame_counter_stats.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <float.h>
#include <string.h>

static void close_to(double actual, double expected) { assert(fabs(actual - expected) < 0.00001); }
int main(void) {
    char clock[160];
    host_frame_counter_clock(clock, sizeof(clock), 1, 300, 17.25);
    assert(strcmp(clock, "game 0:10:00 | render frame 300\nwall 00:17.25") == 0);
    host_frame_counter_clock(clock, sizeof(clock), 1, 1829, 81.5);
    assert(strcmp(clock, "game 1:00:29 | render frame 1829\nwall 01:21.50") == 0);
    host_frame_counter_clock(clock, sizeof(clock), 0, 300, 17.25);
    assert(strcmp(clock, "wall 00:17.25 | render frame 300") == 0);
    char short_clock[5];
    host_frame_counter_clock(short_clock, sizeof(short_clock), 1, 300, 17.25);
    assert(short_clock[sizeof(short_clock) - 1] == '\0');
    struct host_frame_counter_stats stats;
    host_frame_counter_stats_reset(&stats);
    struct host_frame_counter_summary value = host_frame_counter_stats_read(&stats);
    assert(value.frames == 0 && value.fps < 0 && value.cpu_ms < 0 && value.gpu_ms < 0);
    host_frame_counter_stats_sample(&stats, 0.010, 0.002, 0.0);
    host_frame_counter_stats_sample(&stats, 0.030, 0.006, 0.004);
    value = host_frame_counter_stats_read(&stats);
    close_to(value.fps, 50.0); close_to(value.average_ms, 20.0); close_to(value.worst_ms, 30.0);
    close_to(value.cpu_ms, 4.0); close_to(value.gpu_ms, 2.0); assert(value.frames == 2);
    host_frame_counter_stats_reset(&stats);
    host_frame_counter_stats_sample(&stats, 0.010, 0.002, 0.004);
    for (unsigned i = 0; i < 3; i++) host_frame_counter_stats_sample(&stats, 0.010, 0.002, 0.0);
    close_to(host_frame_counter_stats_read(&stats).gpu_ms, 1.0);
    host_frame_counter_stats_reset(&stats);
    host_frame_counter_stats_sample(&stats, 0.010, 0.002, 0.0);
    assert(host_frame_counter_stats_read(&stats).gpu_ms < 0);
    host_frame_counter_stats_reset(&stats);
    host_frame_counter_stats_sample(&stats, NAN, 0.002, 0.003);
    host_frame_counter_stats_sample(&stats, INFINITY, 0.002, 0.003);
    host_frame_counter_stats_sample(&stats, DBL_MAX, 0.002, 0.003);
    host_frame_counter_stats_sample(&stats, -0.001, 0.002, 0.003);
    host_frame_counter_stats_sample(&stats, 0.010, NAN, -1.0);
    value = host_frame_counter_stats_read(&stats);
    assert(value.frames == 1 && value.cpu_ms < 0 && value.gpu_ms < 0);
    for (unsigned i = 0; i < 10000; i++) host_frame_counter_stats_sample(&stats, 0.010, 0.001, 0.002);
    value = host_frame_counter_stats_read(&stats);
    assert(value.frames == HOST_FRAME_COUNTER_SAMPLES_MAX && isfinite(value.fps));
    host_frame_counter_stats_reset(&stats);
    value = host_frame_counter_stats_read(&stats);
    assert(value.frames == 0 && value.fps < 0 && value.gpu_ms < 0);
    puts("frame counter: averages, invalid/unavailable timings, bounded accumulation and reset passed");
    return 0;
}
