#include "host_frame_counter_stats.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

void host_frame_counter_clock(char *buffer, size_t size, int fixed, unsigned long frame, double elapsed) {
    unsigned long minutes = (unsigned long)elapsed / 60;
    if (fixed)
        snprintf(buffer, size, "game %lu:%02lu:%02lu | render frame %lu\nwall %02lu:%05.2f",
                 frame / 1800, (frame / 30) % 60, frame % 30, frame, minutes, elapsed - 60 * minutes);
    else
        snprintf(buffer, size, "wall %02lu:%05.2f | render frame %lu", minutes, elapsed - 60 * minutes, frame);
}

/* Reject impossible timestamps as well as unavailable (-1/0) timing samples. */
static int valid_seconds(double seconds) { return isfinite(seconds) && seconds > 0.0 && seconds <= 3600.0; }
void host_frame_counter_stats_reset(struct host_frame_counter_stats *stats) { memset(stats, 0, sizeof(*stats)); }
void host_frame_counter_stats_sample(struct host_frame_counter_stats *stats, double interval, double cpu, double gpu) {
    if (!valid_seconds(interval) || stats->frames == HOST_FRAME_COUNTER_SAMPLES_MAX) return;
    stats->frames++;
    stats->seconds += interval;
    if (interval > stats->worst_seconds) stats->worst_seconds = interval;
    if (valid_seconds(cpu)) { stats->cpu_frames++; stats->cpu_seconds += cpu; }
    /* Zero means no completed GPU work arrived at this Present. It still
       contributes a frame to the asynchronous work-per-frame denominator. */
    if (isfinite(gpu) && gpu >= 0.0 && gpu <= 3600.0) { stats->gpu_frames++; stats->gpu_seconds += gpu; }
}
struct host_frame_counter_summary host_frame_counter_stats_read(const struct host_frame_counter_stats *stats) {
    return (struct host_frame_counter_summary){
        stats->frames,
        stats->frames ? stats->frames / stats->seconds : -1.0,
        stats->frames ? 1000.0 * stats->seconds / stats->frames : -1.0,
        stats->frames ? 1000.0 * stats->worst_seconds : -1.0,
        stats->cpu_frames ? 1000.0 * stats->cpu_seconds / stats->cpu_frames : -1.0,
        stats->gpu_frames && stats->gpu_seconds > 0.0 ? 1000.0 * stats->gpu_seconds / stats->gpu_frames : -1.0
    };
}
