/* The device benchmark's measurements for one run: every frame's interval,
   CPU time and GPU time, once-a-second samples of memory and thermal state,
   and shader compiles, turned into the summary a report carries. Plain C with
   no platform calls, so the same code runs in the app on every device and in
   tools/ios_test.py's probe (port/ios/tests/benchmark_probe.c). */
#ifndef HALO_BENCHMARK_METRICS_H
#define HALO_BENCHMARK_METRICS_H

#include <stddef.h>
#include <stdint.h>

/* frames slower than this are stalls a player would notice */
#define BENCHMARK_STALL_MILLISECONDS 50.0
#define BENCHMARK_STALLS_KEPT 32
#define BENCHMARK_THERMAL_CHANGES_KEPT 16

/* NSProcessInfoThermalState's values */
enum {
    BENCHMARK_THERMAL_NOMINAL, BENCHMARK_THERMAL_FAIR, BENCHMARK_THERMAL_SERIOUS, BENCHMARK_THERMAL_CRITICAL,
    BENCHMARK_THERMAL_UNKNOWN = -1
};

struct benchmark_metrics {
    /* per frame, in tenths of a millisecond (65535 = 6.5 s or longer) */
    uint16_t *interval, *cpu, *gpu;
    size_t frames, capacity;
    double refresh_hertz, seconds;
    struct { double at, milliseconds; } stalls[BENCHMARK_STALLS_KEPT];
    size_t stall_count;
    /* memory, in megabytes; -1 where never sampled */
    long footprint_first, footprint_last, footprint_peak, left_least;
    size_t samples;
    int thermal_first, thermal_worst, thermal_last;
    struct { double at; int state; } thermal_changes[BENCHMARK_THERMAL_CHANGES_KEPT];
    size_t thermal_change_count;
    unsigned long compiles;
    double compile_milliseconds, compile_longest;
};

struct benchmark_distribution { double mean, median, high, higher, worst; };   /* high = 95th, higher = 99th */

struct benchmark_summary {
    size_t frames;
    double seconds, frames_a_second, refresh_hertz;
    struct benchmark_distribution interval, cpu, gpu;
    /* frames shown for 1, 2, 3, 4 and 5 or more refreshes (index 0 unused) */
    unsigned long shown[6];
    /* the share of frames shown for one refresh, and for one or two */
    double on_time_full, on_time_half;
    size_t stalls;
};

void benchmark_metrics_reset(struct benchmark_metrics *metrics, double refresh_hertz);
void benchmark_metrics_free(struct benchmark_metrics *metrics);
/* one presented frame: milliseconds since the frame before it was presented, and the CPU and GPU time it took
   (negative where unknown) */
void benchmark_metrics_frame(struct benchmark_metrics *metrics, double interval, double cpu, double gpu);
/* a once-a-second sample; megabytes negative where unknown */
void benchmark_metrics_sample(struct benchmark_metrics *metrics, long footprint, long left, int thermal);
void benchmark_metrics_compile(struct benchmark_metrics *metrics, double milliseconds);
void benchmark_metrics_summarize(const struct benchmark_metrics *metrics, struct benchmark_summary *summary);

/* how many refreshes an interval kept its frame on screen, at least 1 */
long benchmark_refreshes(double interval, double refresh_hertz);
/* the run as one JSON object's members (no braces), for a report: summary, memory, thermal, compiles, stalls,
   and each frame's interval, CPU and GPU time as base64 of little-endian 16-bit tenths of a millisecond.
   Returns malloc'd text, or NULL. */
char *benchmark_metrics_json(const struct benchmark_metrics *metrics);
/* base64 of bytes, as malloc'd text */
char *benchmark_base64(const void *bytes, size_t size);

#endif
