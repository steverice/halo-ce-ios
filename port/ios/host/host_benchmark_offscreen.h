/* debug.benchmark_offscreen's rules (host_benchmark.m, gpu_metal.m): plain C, so
   port/ios/tests/benchmark_offscreen_probe.c runs them natively.

   The mode is decided once, at launch: the setting on and a suite named in
   debug.benchmark_suite, off the stereo path. The guest then forces
   debug.fixed_timestep (xbox_kernel.c) and every frame drawn while the suite
   runs goes into a texture that is never shown, with no drawable, Present or
   display wait. */
#ifndef HALO_HOST_BENCHMARK_OFFSCREEN_H
#define HALO_HOST_BENCHMARK_OFFSCREEN_H

#include <string.h>

enum host_benchmark_offscreen {
    HOST_BENCHMARK_OFFSCREEN_OFF,        /* the setting is off */
    HOST_BENCHMARK_OFFSCREEN_ON,
    HOST_BENCHMARK_OFFSCREEN_NO_SUITE,   /* on, with no suite named at launch: ignored */
    HOST_BENCHMARK_OFFSCREEN_STEREO,     /* on, on Vision Pro, whose frames Compositor Services drives: ignored */
};

static inline enum host_benchmark_offscreen host_benchmark_offscreen_decide(int setting, int suite_named, int stereo) {
    if (!setting) return HOST_BENCHMARK_OFFSCREEN_OFF;
    if (stereo) return HOST_BENCHMARK_OFFSCREEN_STEREO;
    return suite_named ? HOST_BENCHMARK_OFFSCREEN_ON : HOST_BENCHMARK_OFFSCREEN_NO_SUITE;
}

/* whether this frame is drawn offscreen: the launch's decision, while a suite runs */
static inline int host_benchmark_offscreen_frame(enum host_benchmark_offscreen decided, int suite_running) {
    return decided == HOST_BENCHMARK_OFFSCREEN_ON && suite_running;
}

/* how a run's frames were paced (the report's pacing_mode and timing_basis): shown on the display; offscreen with
   each visibility answer waited for, so the CPU and the GPU take turns (debug.benchmark_offscreen_mode "serialized");
   offscreen with answers from frames before, so frames overlap ("pipelined"); or offscreen in real time against a
   virtual display (debug.benchmark_virtual_display) */
enum host_benchmark_pacing {
    HOST_BENCHMARK_PACING_PRESENTED,
    HOST_BENCHMARK_PACING_SERIALIZED,
    HOST_BENCHMARK_PACING_PIPELINED,
    HOST_BENCHMARK_PACING_VIRTUAL_DISPLAY,
};

/* the pacing of a run: offscreen (an uncapped run of an offscreen launch), the launch's mode and whether a presented
   run of an offscreen launch goes to the virtual display */
static inline enum host_benchmark_pacing host_benchmark_pacing_of(int offscreen_launch, int offscreen_run, int pipelined,
    int virtual_display) {
    if (!offscreen_launch) return HOST_BENCHMARK_PACING_PRESENTED;
    if (offscreen_run) return pipelined ? HOST_BENCHMARK_PACING_PIPELINED : HOST_BENCHMARK_PACING_SERIALIZED;
    return virtual_display ? HOST_BENCHMARK_PACING_VIRTUAL_DISPLAY : HOST_BENCHMARK_PACING_PRESENTED;
}

/* the run's timing_basis in the report (0 and 1 as they were: presented, and offscreen serialized) */
static inline const char *host_benchmark_timing_basis(int pacing) {
    switch (pacing) {
    case HOST_BENCHMARK_PACING_SERIALIZED: return "offscreen_uncapped";
    case HOST_BENCHMARK_PACING_PIPELINED: return "offscreen_pipelined";
    case HOST_BENCHMARK_PACING_VIRTUAL_DISPLAY: return "virtual_display";
    default: return "submission_timing";
    }
}

/* the run's pacing_mode in the report */
static inline const char *host_benchmark_pacing_mode(int pacing) {
    switch (pacing) {
    case HOST_BENCHMARK_PACING_SERIALIZED: return "offscreen_serialized";
    case HOST_BENCHMARK_PACING_PIPELINED: return "offscreen_pipelined";
    case HOST_BENCHMARK_PACING_VIRTUAL_DISPLAY: return "virtual_display";
    default: return "presented";
    }
}

/* debug.benchmark_offscreen_mode: 1 for "pipelined", 0 for "serialized" (and anything else, which the log names) */
static inline int host_benchmark_offscreen_mode_pipelined(const char *value) {
    return value && !strcmp(value, "pipelined");
}

#endif
