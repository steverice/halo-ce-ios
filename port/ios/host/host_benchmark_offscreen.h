/* debug.benchmark_offscreen's rules (host_benchmark.m, gpu_metal.m): plain C, so
   port/ios/tests/benchmark_offscreen_probe.c runs them natively.

   The mode is decided once, at launch: the setting on and a suite named in
   debug.benchmark_suite, off the stereo path. The guest then forces
   debug.fixed_timestep (xbox_kernel.c) and every frame drawn while the suite
   runs goes into a texture that is never shown, with no drawable, Present or
   display wait. */
#ifndef HALO_HOST_BENCHMARK_OFFSCREEN_H
#define HALO_HOST_BENCHMARK_OFFSCREEN_H

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

/* the run's timing_basis in the report */
static inline const char *host_benchmark_timing_basis(int offscreen) {
    return offscreen ? "offscreen_uncapped" : "submission_timing";
}

#endif
