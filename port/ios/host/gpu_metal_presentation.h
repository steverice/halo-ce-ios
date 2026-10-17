/* Primary drawable presentation observations, independent of frame pacing. */
#ifndef HALO_GPU_METAL_PRESENTATION_H
#define HALO_GPU_METAL_PRESENTATION_H
#import <QuartzCore/CAMetalLayer.h>
#include <TargetConditionals.h>
#include "host_benchmark.h"

static inline void gpu_metal_presentation_configure(CAMetalLayer *primary_layer)
{
#if TARGET_OS_VISION
    (void)primary_layer;
    host_benchmark_presentation_configure(HOST_PRESENTATION_VISIONOS, -1, -1);
#elif TARGET_OS_SIMULATOR
    (void)primary_layer;
    host_benchmark_presentation_configure(HOST_PRESENTATION_SIMULATOR, -1, -1);
#else
    int synchronized_display = -1, allows_tearing = -1;
#if TARGET_OS_MACCATALYST
    if (@available(macCatalyst 13.1, *))
    {
        synchronized_display = primary_layer.displaySyncEnabled;
        allows_tearing = !synchronized_display;
    }
#else
    (void)primary_layer;
#endif
    /* Configuration records policy changes without resetting an active run.
       Unsynchronized observations need not describe whole refresh images. */
    host_benchmark_presentation_configure(HOST_PRESENTATION_METAL, synchronized_display, allows_tearing);
#endif
}

static inline void gpu_metal_presentation_attempt(int cutscene, int transition)
{
#if TARGET_OS_SIMULATOR || TARGET_OS_VISION
    (void)cutscene; (void)transition;
#else
    if (transition) host_benchmark_presentation_exclude();
    else host_benchmark_presentation_scene(cutscene);
#endif
}

/* Call only for the primary drawable, with transition captured before the
   legacy frame log consumes it. Companion outputs never enter this path. */
static inline void gpu_metal_presentation_observe(id<CAMetalDrawable> drawable, double submitted,
    int cutscene, int transition)
{
#if TARGET_OS_SIMULATOR || TARGET_OS_VISION
    (void)drawable; (void)submitted; (void)cutscene; (void)transition;
#else
    if (transition)
        return;
    struct host_benchmark_presentation_token token = host_benchmark_presentation_reserve(submitted, cutscene);
    if (token.generation)
        [drawable addPresentedHandler:^(id<MTLDrawable> shown)
        {
            host_benchmark_presentation_complete(token, shown.presentedTime);
        }];
#endif
}
#endif
