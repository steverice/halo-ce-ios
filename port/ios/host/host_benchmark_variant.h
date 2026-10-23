/* The signed benchmark catalog's display variants (host_benchmark_catalog.m reads them; tools/benchmark_catalog.py
   VARIANT_SETTINGS has the same rules): display configurations a device of a named model runs the whole full-unified
   suite under, once each, in list order, from one start (host_benchmark.m). Plain C, for
   port/ios/tests/benchmark_variant_probe.c. */
#pragma once
#include <stdio.h>

#define HOST_BENCHMARK_VARIANTS_MAXIMUM 4
#define HOST_BENCHMARK_VARIANT_MODELS_MAXIMUM 8
enum { HOST_BENCHMARK_UPSCALER_UNSET = -1, HOST_BENCHMARK_UPSCALER_BILINEAR = 0, HOST_BENCHMARK_UPSCALER_METALFX = 1 };

struct host_benchmark_variant {
    char id[48];
    char models[HOST_BENCHMARK_VARIANT_MODELS_MAXIMUM][24];
    int model_count;
    /* -1 where the variant leaves the setting as the device has it */
    int render_height, output_height, upscaler;
};

/* the built-in output cap a pass wants (0: none): its own, else the one the app started with */
static inline int host_benchmark_variant_cap(const struct host_benchmark_variant *variant, int launch_cap)
{
    return variant->output_height >= 0 ? variant->output_height : launch_cap;
}

/* the upscaler a pass wants (0 bilinear, 1 MetalFX): its own, else the one the app started with */
static inline int host_benchmark_variant_upscaler(const struct host_benchmark_variant *variant, int launch_upscaler)
{
    return variant->upscaler >= 0 ? variant->upscaler : launch_upscaler;
}

/* whether pass index of count is the last one: the end of the list, or a suite asked to finish */
static inline int host_benchmark_variant_last_pass(int index, int count, int finish_requested)
{
    return finish_requested || index + 1 >= count;
}

/* the file a pass's report is kept in, beside report.json: variant-00.json, variant-01.json, ... */
static inline void host_benchmark_variant_report_name(int index, char *name, size_t size)
{
    snprintf(name, size, "variant-%02d.json", index);
}

/* a pass index read back from the suite's state, or 0 when it is not one of count passes */
static inline int host_benchmark_variant_resumed_index(long stored, int count)
{
    return stored >= 0 && stored < count ? (int)stored : 0;
}
