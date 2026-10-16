#ifndef HOST_FRAME_COUNTER_STATS_H
#define HOST_FRAME_COUNTER_STATS_H

#include <stddef.h>

/* Fixed mode retains the 30 Hz game timecode; wall time remains independent. */
void host_frame_counter_clock(char *buffer, size_t size, int fixed, unsigned long frame, double elapsed);

#define HOST_FRAME_COUNTER_SAMPLES_MAX 4096u
struct host_frame_counter_stats {
    unsigned frames, cpu_frames, gpu_frames;
    double seconds, worst_seconds, cpu_seconds, gpu_seconds;
};
struct host_frame_counter_summary {
    unsigned frames;
    double fps, average_ms, worst_ms, cpu_ms, gpu_ms;
};
void host_frame_counter_stats_reset(struct host_frame_counter_stats *stats);
void host_frame_counter_stats_sample(struct host_frame_counter_stats *stats, double interval, double cpu, double gpu);
struct host_frame_counter_summary host_frame_counter_stats_read(const struct host_frame_counter_stats *stats);
#endif
