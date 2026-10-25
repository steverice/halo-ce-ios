#import <CoreML/CoreML.h>
#include "texture_upscale_model.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

/* One enumerated shape avoids retaining an ANE program for each combination of texture dimensions.
   The smaller 256-texel core leaves room for the generator's full-resolution output and refine buffers. */
enum { MODEL_SIDE = 288, MODEL_CONTEXT = 16, MODEL_CORE = MODEL_SIDE - 2 * MODEL_CONTEXT, MODEL_SCALE = 4 };

/* Two sets of tile buffers: the CPU packs and consumes one call's tiles while the Neural Engine runs the other's. */
@interface TextureUpscaleModel : NSObject
{
@public
    MLModel *model;
    uint32_t planes;    /* channels per call: 3 (S4g on each, as a batch) or 1 */
    MLMultiArray *input[2];
    MLMultiArray *backing[2];
    MLDictionaryFeatureProvider *provider[2];
    MLPredictionOptions *options[2];
    dispatch_queue_t consumers;
    struct texture_upscale_model_stats stats;
    texture_upscale_model_observer observer;
    void *observer_context;
}
@end
@implementation TextureUpscaleModel
@end

static TextureUpscaleModel *active_model;
static const volatile int *cancel_flag;

void texture_upscale_model_set_cancel(const volatile int *flag) { cancel_flag = flag; }
int texture_upscale_model_cancelled(void) { return cancel_flag && *cancel_flag; }

static void set_error(char *error, size_t size, NSString *message)
{
    if (error && size) snprintf(error, size, "%s", (message ?: @"Core ML inference failed").UTF8String);
}

static uint64_t footprint(uint64_t *peak)
{
    task_vm_info_data_t info = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    if (peak) *peak = info.ledger_phys_footprint_peak;
    return info.phys_footprint;
}

static uint32_t addressed(int64_t index, uint32_t size, int wrap)
{
    if (wrap) { index %= size; return (uint32_t)(index < 0 ? index + size : index); }
    return index < 0 ? 0 : index >= size ? size - 1 : (uint32_t)index;
}

/* Writes one plane of the model's planar input tile from channel channel of the interleaved source. */
static void pack_tile(float *tile, const float *source, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t channel, uint32_t tx, uint32_t ty, uint32_t core_w, uint32_t core_h, int wrap)
{
    uint32_t xs[MODEL_SIDE];
    for (uint32_t x = 0; x < MODEL_SIDE; x++) {
        /* Match coreml._predictor: after the real contextual rectangle, repeat its final texel. */
        uint32_t local = x < core_w + 2 * MODEL_CONTEXT ? x : core_w + 2 * MODEL_CONTEXT - 1;
        xs[x] = addressed((int64_t)tx + local - MODEL_CONTEXT, width, wrap) * channels + channel;
    }
    for (uint32_t y = 0; y < MODEL_SIDE; y++) {
        uint32_t local = y < core_h + 2 * MODEL_CONTEXT ? y : core_h + 2 * MODEL_CONTEXT - 1;
        uint32_t sy = addressed((int64_t)ty + local - MODEL_CONTEXT, height, wrap);
        const float *row = source + (size_t)sy * width * channels;
        float *out = tile + y * MODEL_SIDE;
        for (uint32_t x = 0; x < MODEL_SIDE; x++) out[x] = row[xs[x]];
    }
}

static MLMultiArray *make_array(uint32_t planes, uint32_t side, MLMultiArrayDataType type, NSError **error)
{
    size_t item_size = type == MLMultiArrayDataTypeFloat16 ? 2 : 4;
    size_t bytes = (size_t)planes * side * side * item_size;
    void *data = NULL;
    if (posix_memalign(&data, (size_t)getpagesize(), bytes)) return nil;
    MLMultiArray *array = [[MLMultiArray alloc] initWithDataPointer:data shape:@[@1, @(planes), @(side), @(side)]
        dataType:type strides:@[@(planes * side * side), @(side * side), @(side), @1]
        deallocator:^(void *pointer) { free(pointer); } error:error];
    if (!array) free(data);
    return array;
}

/* Development only: HALO_TEXTURE_UPSCALE_MODEL names a compiled package to use instead of the bundled one. */
static NSURL *model_url(void)
{
    const char *override = getenv("HALO_TEXTURE_UPSCALE_MODEL");
    if (override && *override) {
        NSString *path = @(override);
        if (!path.isAbsolutePath)
            path = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject
                stringByAppendingPathComponent:path];
        if ([NSFileManager.defaultManager fileExistsAtPath:path]) return [NSURL fileURLWithPath:path];
    }
    return [[NSBundle mainBundle] URLForResource:@"S4g" withExtension:@"mlmodelc"];
}

int texture_upscale_model_available(void)
{
    @autoreleasepool {
        if (@available(iOS 17.0, tvOS 17.0, macOS 14.0, *)) {
            if (!model_url()) return 0;
            for (id<MLComputeDeviceProtocol> device in MLModel.availableComputeDevices)
                if ([device isKindOfClass:MLNeuralEngineComputeDevice.class]) return 1;
        }
        return 0;
    }
}

struct texture_upscale_model *texture_upscale_model_create(char *error, size_t error_size)
{
    @autoreleasepool {
        if (error && error_size) error[0] = 0;
        @synchronized(TextureUpscaleModel.class) {
            if (active_model) { set_error(error, error_size, @"The texture model is already in use"); return NULL; }
            if (!texture_upscale_model_available()) {
                set_error(error, error_size, @"S4g requires a compiled model and a Neural Engine available to Core ML");
                return NULL;
            }
            TextureUpscaleModel *handle = [TextureUpscaleModel new];
            handle->stats.input_width = handle->stats.input_height = MODEL_SIDE;
            handle->stats.footprint_before = footprint(&handle->stats.peak_footprint);
            MLModelConfiguration *configuration = [MLModelConfiguration new];
            configuration.computeUnits = MLComputeUnitsCPUAndNeuralEngine;
            NSError *failure = nil;
            NSURL *url = model_url();
            handle->model = [MLModel modelWithContentsOfURL:url configuration:configuration error:&failure];
            if (!handle->model) { set_error(error, error_size, failure.localizedDescription); return NULL; }
            NSArray<NSNumber *> *shape = handle->model.modelDescription.inputDescriptionsByName[@"image"].multiArrayConstraint.shape;
            handle->planes = shape.count == 4 ? shape[1].unsignedIntValue : 0;
            if (handle->planes != 1 && handle->planes != 3) { set_error(error, error_size, @"S4g has an unsupported input shape"); return NULL; }
            MLMultiArrayDataType output_type = handle->model.modelDescription.outputDescriptionsByName[@"upscaled"].multiArrayConstraint.dataType;
            if (output_type != MLMultiArrayDataTypeFloat16 && output_type != MLMultiArrayDataTypeFloat32) {
                set_error(error, error_size, @"S4g has an unsupported output data type"); return NULL;
            }
            for (int i = 0; i < 2; i++) {
                handle->input[i] = make_array(handle->planes, MODEL_SIDE, MLMultiArrayDataTypeFloat32, &failure);
                handle->backing[i] = make_array(handle->planes, MODEL_SIDE * MODEL_SCALE, output_type, &failure);
                if (!handle->input[i] || !handle->backing[i]) {
                    set_error(error, error_size, failure.localizedDescription ?: @"Could not allocate S4g tile buffers"); return NULL;
                }
                handle->provider[i] = [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{@"image": handle->input[i]} error:&failure];
                if (!handle->provider[i]) { set_error(error, error_size, failure.localizedDescription); return NULL; }
                handle->options[i] = [MLPredictionOptions new];
                handle->options[i].outputBackings = @{@"upscaled": handle->backing[i]};
            }
            handle->consumers = dispatch_queue_create("org.visr.texture-upscale.tiles",
                dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, qos_class_self(), 0));
            active_model = handle;
            return (__bridge_retained struct texture_upscale_model *)handle;
        }
    }
}

void texture_upscale_model_destroy(struct texture_upscale_model *model)
{
    if (!model) return;
    @autoreleasepool {
        @synchronized(TextureUpscaleModel.class) {
            TextureUpscaleModel *handle = (__bridge_transfer TextureUpscaleModel *)model;
            NSLog(@"texture-upscale: S4g shape %ux%u before=%llu after=%llu peak=%llu calls=%llu",
                MODEL_SIDE, MODEL_SIDE, (unsigned long long)handle->stats.footprint_before,
                (unsigned long long)handle->stats.footprint_after, (unsigned long long)handle->stats.peak_footprint,
                (unsigned long long)handle->stats.predictions);
            if (active_model == handle) active_model = nil;
        }
    }
}

void texture_upscale_model_set_observer(struct texture_upscale_model *model,
    texture_upscale_model_observer observer, void *context)
{
    if (!model) return;
    TextureUpscaleModel *handle = (__bridge TextureUpscaleModel *)model;
    handle->observer = observer;
    handle->observer_context = context;
}

void texture_upscale_model_get_stats(struct texture_upscale_model *model, struct texture_upscale_model_stats *stats)
{
    if (!stats) return;
    *stats = model ? ((__bridge TextureUpscaleModel *)model)->stats : (struct texture_upscale_model_stats){0};
}


struct tile_slot { uint32_t tx, ty, cw, ch, channel; };

/* Work units are (tile, channel) pairs in tile order; each call takes as many as the package has planes, so three
   channels of one tile go through a three-plane package together, and three tiles' single channel share a call. The
   previous call's units are handed to the consumer on another thread while the Neural Engine runs this one. */
int texture_upscale_model_predict_tiles(struct texture_upscale_model *model, const float *input,
    uint32_t width, uint32_t height, uint32_t channels, int wrap,
    texture_upscale_tile_consumer consumer, void *context, char *error, size_t error_size)
{
    if (error && error_size) error[0] = 0;
    if (!model || !input || !consumer || !width || !height || (channels != 1 && channels != 3) ||
        width > UINT32_MAX / MODEL_SCALE || height > UINT32_MAX / MODEL_SCALE) {
        set_error(error, error_size, @"Invalid S4g image or output dimensions"); return -1;
    }
    TextureUpscaleModel *handle = (__bridge TextureUpscaleModel *)model;
    uint32_t columns = (width + MODEL_CORE - 1) / MODEL_CORE, rows = (height + MODEL_CORE - 1) / MODEL_CORE;
    uint64_t units = (uint64_t)columns * rows * channels;
    __block _Atomic int consumer_failed = 0;
    int status = 0;
    dispatch_group_t pending[2] = {dispatch_group_create(), dispatch_group_create()};
    for (uint64_t first = 0, call = 0; first < units && !status && !consumer_failed; first += handle->planes, call++) {
        int buffer = (int)(call & 1);
        /* The consumer of the call before last still reads this buffer's output. */
        dispatch_group_wait(pending[buffer], DISPATCH_TIME_FOREVER);
        if (consumer_failed) break;
        if (texture_upscale_model_cancelled()) {
            set_error(error, error_size, @"S4g stopped for gameplay"); status = -1; break;
        }
        struct tile_slot slots[3];
        unsigned used = 0;
        uint64_t pack_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        float *tile = handle->input[buffer].dataPointer;
        for (uint64_t u = first; u < units && used < handle->planes; u++, used++) {
            uint64_t t = u / channels;
            uint32_t tx = (uint32_t)(t % columns) * MODEL_CORE, ty = (uint32_t)(t / columns) * MODEL_CORE;
            slots[used] = (struct tile_slot){tx, ty, width - tx < MODEL_CORE ? width - tx : MODEL_CORE,
                height - ty < MODEL_CORE ? height - ty : MODEL_CORE, (uint32_t)(u % channels)};
            pack_tile(tile + (size_t)used * MODEL_SIDE * MODEL_SIDE, input, width, height, channels,
                slots[used].channel, tx, ty, slots[used].cw, slots[used].ch, wrap);
        }
        handle->stats.pack_seconds += (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - pack_start) / 1e9;
        @autoreleasepool {
            uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            NSError *failure = nil;
            id<MLFeatureProvider> prediction = [handle->model predictionFromFeatures:handle->provider[buffer]
                options:handle->options[buffer] error:&failure];
            handle->stats.seconds += (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1e9;
            /* Core ML sometimes declines a proposed backing; the block keeps its own output alive until consumed. */
            MLMultiArray *result = [prediction featureValueForName:@"upscaled"].multiArrayValue;
            NSArray<NSNumber *> *shape = result.shape;
            if (!result || shape.count != 4 || shape[0].unsignedIntValue != 1 || shape[1].unsignedIntValue != handle->planes ||
                shape[2].unsignedIntValue != MODEL_SIDE * MODEL_SCALE || shape[3].unsignedIntValue != MODEL_SIDE * MODEL_SCALE ||
                (result.dataType != MLMultiArrayDataTypeFloat16 && result.dataType != MLMultiArrayDataTypeFloat32)) {
                set_error(error, error_size, failure.localizedDescription ?: @"S4g returned an invalid output tile"); status = -1;
            } else {
                handle->stats.predictions++;
                handle->stats.tiles += used;
                struct { struct tile_slot slot[3]; } batch;
                memcpy(batch.slot, slots, sizeof(batch.slot));
                unsigned count = used;
                dispatch_group_async(pending[buffer], handle->consumers, ^{
                    uint64_t consume_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                    ptrdiff_t sc = result.strides[1].longLongValue, sy = result.strides[2].longLongValue,
                        sx = result.strides[3].longLongValue;
                    int half = result.dataType == MLMultiArrayDataTypeFloat16;
                    size_t origin = (size_t)(MODEL_CONTEXT * MODEL_SCALE) * (sy + sx);
                    for (unsigned slot = 0; slot < count && !consumer_failed; slot++) {
                        struct texture_upscale_tile view = {batch.slot[slot].tx, batch.slot[slot].ty, batch.slot[slot].cw, batch.slot[slot].ch,
                            half ? (const void *)((const _Float16 *)result.dataPointer + slot * sc + origin) :
                                (const void *)((const float *)result.dataPointer + slot * sc + origin), sy, sx, half};
                        if (consumer(context, &view, batch.slot[slot].channel)) consumer_failed = 1;
                    }
                    handle->stats.consume_seconds += (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - consume_start) / 1e9;
                });
            }
        }
        handle->stats.footprint_after = footprint(&handle->stats.peak_footprint);
        if (handle->stats.predictions == 1)
            NSLog(@"texture-upscale: S4g shape %ux%u x %u before=%llu after=%llu peak=%llu",
                MODEL_SIDE, MODEL_SIDE, handle->planes, (unsigned long long)handle->stats.footprint_before,
                (unsigned long long)handle->stats.footprint_after, (unsigned long long)handle->stats.peak_footprint);
        if (handle->observer) handle->observer(&handle->stats, handle->observer_context);
    }
    dispatch_group_wait(pending[0], DISPATCH_TIME_FOREVER);
    dispatch_group_wait(pending[1], DISPATCH_TIME_FOREVER);
    if (!status && consumer_failed) { set_error(error, error_size, @"S4g tile consumer failed"); status = -1; }
    return status;
}

struct strided_output { float *output; uint32_t width, stride; };
static int write_strided(void *context, const struct texture_upscale_tile *tile, uint32_t channel)
{
    struct strided_output *out = context;
    size_t ow = (size_t)out->width * MODEL_SCALE;
    for (uint32_t y = 0; y < tile->height * MODEL_SCALE; y++) {
        float *row = out->output + (((size_t)tile->y * MODEL_SCALE + y) * ow + (size_t)tile->x * MODEL_SCALE) * out->stride + channel;
        if (tile->half && tile->column_stride == 1) {
            const _Float16 *source = (const _Float16 *)tile->data + (ptrdiff_t)y * tile->row_stride;
            for (uint32_t x = 0; x < tile->width * MODEL_SCALE; x++) row[(size_t)x * out->stride] = source[x];
        } else for (uint32_t x = 0; x < tile->width * MODEL_SCALE; x++)
            row[(size_t)x * out->stride] = texture_upscale_tile_at(tile, (int32_t)x, (int32_t)y);
    }
    return 0;
}

int texture_upscale_model_predict4x(struct texture_upscale_model *model, const float *input,
    uint32_t width, uint32_t height, uint32_t channels, int wrap, float *output,
    char *error, size_t error_size)
{
    return texture_upscale_model_predict4x_strided(model, input, width, height, channels, wrap, output,
        channels, error, error_size);
}

int texture_upscale_model_predict4x_strided(struct texture_upscale_model *model, const float *input,
    uint32_t width, uint32_t height, uint32_t channels, int wrap, float *output,
    uint32_t output_pixel_stride, char *error, size_t error_size)
{
    if (error && error_size) error[0] = 0;
    if (!output || output_pixel_stride < channels || (channels != 1 && channels != 3) || !width || !height ||
        width > UINT32_MAX / MODEL_SCALE || height > UINT32_MAX / MODEL_SCALE ||
        (size_t)width > SIZE_MAX / height / output_pixel_stride / (MODEL_SCALE * MODEL_SCALE) / sizeof(float)) {
        set_error(error, error_size, @"Invalid S4g image or output dimensions"); return -1;
    }
    struct strided_output out = {output, width, output_pixel_stride};
    return texture_upscale_model_predict_tiles(model, input, width, height, channels, wrap, write_strided, &out,
        error, error_size);
}
