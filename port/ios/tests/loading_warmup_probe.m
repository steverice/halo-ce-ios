/* The synchronous backend's selection, dispatch, and publication boundary is
   inserted verbatim by test_loading_progress.py. Metal compilation itself is
   replaced by deterministic results, including a failed published pipeline. */
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#include <assert.h>
#include "metal_compile_progress.h"

@interface MetalShader : NSObject
@end
@implementation MetalShader
@end
struct pipeline_key { uint32_t vertex_shader, pixel_shader; };
static int shaders, pipeline_table;
static MetalShader *shader;
static struct metal_compile_progress loading_compiles = { .lock = PTHREAD_MUTEX_INITIALIZER };
static int expect_total, supersede, publish_success;
static struct gpu_compile_progress snapshot(void) {
    struct gpu_compile_progress result;
    metal_compile_progress_update(&loading_compiles, GPU_COMPILE_PROGRESS_SNAPSHOT, &result);
    return result;
}
static void pipeline_key_from_description(const struct gpu_pipeline_description *description, struct pipeline_key *key) {
    *key = (struct pipeline_key){description->vertex_shader, description->pixel_shader};
}
static MetalShader *table_get(int table, uint32_t handle) { (void)table; return handle ? shader : nil; }
static id metal_compile_lookup(int *table, const void *data, size_t size) {
    (void)table; (void)size;
    return ((const struct pipeline_key *)data)->vertex_shader == 42 ? shader : nil;
}
static id pipeline_descriptor(const struct pipeline_key *key, MetalShader *vertex, MetalShader *pixel, int play) {
    (void)vertex; (void)pixel; (void)play;
    return @(key->vertex_shader);
}
static id pipeline_make(NSNumber *descriptor) {
    assert(snapshot().total == (uint32_t)expect_total);
    if (supersede) {
        struct gpu_compile_progress progress;
        metal_compile_progress_update(&loading_compiles, GPU_COMPILE_PROGRESS_BEGIN, &progress);
    }
    return descriptor.intValue == 2 ? [NSNull null] : shader;
}
static id pipeline_publish(const struct pipeline_key *key, id pipeline) {
    (void)key;
    return publish_success ? shader : pipeline;
}
static void warm_pipelines(const struct gpu_pipeline_description *list, unsigned long count) {
    unsigned long index;
    NSMutableArray *names = [NSMutableArray array], *descriptors = [NSMutableArray array];
    NSMutableSet *named = [NSMutableSet set];
    NSMutableArray *made;
    /* PRODUCTION_WARMUP */
}
int main(void) {
    @autoreleasepool {
        shader = [MetalShader new];
        struct gpu_compile_progress progress;
        struct gpu_pipeline_description list[4] = {
            {.vertex_shader=1, .pixel_shader=1}, {.vertex_shader=1, .pixel_shader=1},
            {.vertex_shader=42, .pixel_shader=1}, {.vertex_shader=2, .pixel_shader=1},
        };
        metal_compile_progress_update(&loading_compiles, GPU_COMPILE_PROGRESS_BEGIN, &progress);
        uint64_t library = metal_compile_progress_schedule(&loading_compiles);
        metal_compile_progress_finish(&loading_compiles, library, 1);
        expect_total = 3; /* one completed library, two unique uncached pipelines */
        warm_pipelines(list, 4);
        progress = snapshot();
        assert(progress.total == 3 && progress.completed == 3 && progress.failed == 1);
        metal_compile_progress_update(&loading_compiles, GPU_COMPILE_PROGRESS_SEAL, &progress);
        assert(progress.sealed && progress.total == 3);
        /* Publication may find a successful pipeline another path already made. */
        metal_compile_progress_update(&loading_compiles, GPU_COMPILE_PROGRESS_BEGIN, &progress);
        publish_success = 1; expect_total = 1;
        warm_pipelines(&list[3], 1);
        progress = snapshot();
        assert(progress.total == 1 && progress.completed == 1 && progress.failed == 0);
        /* Completion carries the token from before dispatch, even if a new load begins. */
        metal_compile_progress_update(&loading_compiles, GPU_COMPILE_PROGRESS_BEGIN, &progress);
        supersede = 1;
        warm_pipelines(list, 1);
        progress = snapshot();
        assert(progress.total == 0 && progress.completed == 0 && progress.failed == 0);
        puts("PASS: synchronous Metal warmup counts unique jobs and published failures by generation");
    }
    return 0;
}
