#import <CoreML/CoreML.h>
#include "texture_upscale_model.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* One enumerated shape avoids retaining an ANE program for each combination of texture dimensions.
   The smaller 256-texel core leaves room for the generator's full-resolution output and refine buffers. */
enum { MODEL_SIDE = 288, MODEL_CONTEXT = 16, MODEL_CORE = MODEL_SIDE - 2 * MODEL_CONTEXT, MODEL_SCALE = 4 };

@interface TextureUpscaleModel : NSObject
{
@public
    MLModel *model;
    MLMultiArray *input;
    MLMultiArray *backing;
    MLDictionaryFeatureProvider *provider;
    MLPredictionOptions *options;
    struct texture_upscale_model_stats stats;
    texture_upscale_model_observer observer;
    void *observer_context;
}
@end
@implementation TextureUpscaleModel
@end

static TextureUpscaleModel *active_model;

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

static void pack_tile(float *tile, const float *source, uint32_t width, uint32_t height,
    uint32_t channels, uint32_t tx, uint32_t ty, uint32_t core_w, uint32_t core_h, int wrap)
{
    uint32_t xs[MODEL_SIDE];
    for (uint32_t x = 0; x < MODEL_SIDE; x++) {
        /* Match coreml._predictor: after the real contextual rectangle, repeat its final texel. */
        uint32_t local = x < core_w + 2 * MODEL_CONTEXT ? x : core_w + 2 * MODEL_CONTEXT - 1;
        xs[x] = addressed((int64_t)tx + local - MODEL_CONTEXT, width, wrap);
    }
    for (uint32_t y = 0; y < MODEL_SIDE; y++) {
        uint32_t local = y < core_h + 2 * MODEL_CONTEXT ? y : core_h + 2 * MODEL_CONTEXT - 1;
        uint32_t sy = addressed((int64_t)ty + local - MODEL_CONTEXT, height, wrap);
        for (uint32_t x = 0; x < MODEL_SIDE; x++) {
            size_t s = ((size_t)sy * width + xs[x]) * channels;
            for (uint32_t c = 0; c < 3; c++)
                tile[(size_t)c * MODEL_SIDE * MODEL_SIDE + y * MODEL_SIDE + x] = source[s + (channels == 1 ? 0 : c)];
        }
    }
}

static MLMultiArray *make_array(uint32_t side, MLMultiArrayDataType type, NSError **error)
{
    size_t item_size = type == MLMultiArrayDataTypeFloat16 ? 2 : type == MLMultiArrayDataTypeDouble ? 8 : 4;
    size_t bytes = 3 * (size_t)side * side * item_size;
    void *data = NULL;
    if (posix_memalign(&data, (size_t)getpagesize(), bytes)) return nil;
    MLMultiArray *array = [[MLMultiArray alloc] initWithDataPointer:data shape:@[@1, @3, @(side), @(side)]
        dataType:type strides:@[@(3 * side * side), @(side * side), @(side), @1]
        deallocator:^(void *pointer) { free(pointer); } error:error];
    if (!array) free(data);
    return array;
}

int texture_upscale_model_available(void)
{
    @autoreleasepool {
        if (@available(iOS 17.0, tvOS 17.0, macOS 14.0, *)) {
            if (![[NSBundle mainBundle] URLForResource:@"S4g" withExtension:@"mlmodelc"]) return 0;
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
            NSURL *url = [[NSBundle mainBundle] URLForResource:@"S4g" withExtension:@"mlmodelc"];
            handle->model = [MLModel modelWithContentsOfURL:url configuration:configuration error:&failure];
            if (!handle->model) { set_error(error, error_size, failure.localizedDescription); return NULL; }
            handle->input = make_array(MODEL_SIDE, MLMultiArrayDataTypeFloat32, &failure);
            MLMultiArrayDataType output_type = handle->model.modelDescription.outputDescriptionsByName[@"upscaled"].multiArrayConstraint.dataType;
            if (output_type != MLMultiArrayDataTypeFloat16 && output_type != MLMultiArrayDataTypeFloat32 &&
                output_type != MLMultiArrayDataTypeDouble) {
                set_error(error, error_size, @"S4g has an unsupported output data type"); return NULL;
            }
            handle->backing = make_array(MODEL_SIDE * MODEL_SCALE, output_type, &failure);
            if (!handle->input || !handle->backing) {
                set_error(error, error_size, failure.localizedDescription ?: @"Could not allocate S4g tile buffers"); return NULL;
            }
            handle->provider = [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{@"image": handle->input} error:&failure];
            if (!handle->provider) { set_error(error, error_size, failure.localizedDescription); return NULL; }
            handle->options = [MLPredictionOptions new];
            handle->options.outputBackings = @{@"upscaled": handle->backing};
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

static float output_value(const void *data, MLMultiArrayDataType type, size_t index)
{
    if (type == MLMultiArrayDataTypeFloat16) return ((const _Float16 *)data)[index];
    if (type == MLMultiArrayDataTypeDouble) return (float)((const double *)data)[index];
    return ((const float *)data)[index];
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
    if (!model || !input || !output || !width || !height || (channels != 1 && channels != 3) ||
        output_pixel_stride < channels ||
        width > UINT32_MAX / MODEL_SCALE || height > UINT32_MAX / MODEL_SCALE ||
        (size_t)width > SIZE_MAX / height / output_pixel_stride / (MODEL_SCALE * MODEL_SCALE) / sizeof(float)) {
        set_error(error, error_size, @"Invalid S4g image or output dimensions"); return -1;
    }
    TextureUpscaleModel *handle = (__bridge TextureUpscaleModel *)model;
    for (uint32_t ty = 0; ty < height; ty += MODEL_CORE) for (uint32_t tx = 0; tx < width; tx += MODEL_CORE) {
        uint32_t cw = width - tx < MODEL_CORE ? width - tx : MODEL_CORE;
        uint32_t ch = height - ty < MODEL_CORE ? height - ty : MODEL_CORE;
        int status = 0;
        /* Core ML sometimes declines a proposed backing. Its temporary output must die after this tile. */
        @autoreleasepool {
            pack_tile(handle->input.dataPointer, input, width, height, channels, tx, ty, cw, ch, wrap);
            uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            NSError *failure = nil;
            id<MLFeatureProvider> prediction = [handle->model predictionFromFeatures:handle->provider options:handle->options error:&failure];
            handle->stats.seconds += (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1e9;
            MLMultiArray *result = [prediction featureValueForName:@"upscaled"].multiArrayValue;
            NSArray<NSNumber *> *shape = result.shape;
            if (!result || shape.count != 4 || shape[0].unsignedIntValue != 1 || shape[1].unsignedIntValue != 3 ||
                shape[2].unsignedIntValue != MODEL_SIDE * MODEL_SCALE || shape[3].unsignedIntValue != MODEL_SIDE * MODEL_SCALE ||
                (result.dataType != MLMultiArrayDataTypeFloat16 && result.dataType != MLMultiArrayDataTypeFloat32 &&
                 result.dataType != MLMultiArrayDataTypeDouble)) {
                set_error(error, error_size, failure.localizedDescription ?: @"S4g returned an invalid output tile"); status = -1;
            } else {
                size_t sc = result.strides[1].unsignedLongLongValue;
                size_t sy = result.strides[2].unsignedLongLongValue, sx = result.strides[3].unsignedLongLongValue;
                const void *data = result.dataPointer;
                for (uint32_t y = 0; y < ch * MODEL_SCALE; y++) for (uint32_t x = 0; x < cw * MODEL_SCALE; x++) {
                    size_t s = (y + MODEL_CONTEXT * MODEL_SCALE) * sy + (x + MODEL_CONTEXT * MODEL_SCALE) * sx;
                    size_t d = (((size_t)ty * MODEL_SCALE + y) * width * MODEL_SCALE + tx * MODEL_SCALE + x) * output_pixel_stride;
                    if (channels == 1) {
                        output[d] = (output_value(data, result.dataType, s) + output_value(data, result.dataType, s + sc) +
                            output_value(data, result.dataType, s + 2 * sc)) / 3;
                    } else for (uint32_t c = 0; c < 3; c++) output[d + c] = output_value(data, result.dataType, s + c * sc);
                }
                handle->stats.predictions++;
            }
        }
        handle->stats.footprint_after = footprint(&handle->stats.peak_footprint);
        if (handle->stats.predictions == 1)
            NSLog(@"texture-upscale: S4g shape %ux%u before=%llu after=%llu peak=%llu",
                MODEL_SIDE, MODEL_SIDE, (unsigned long long)handle->stats.footprint_before,
                (unsigned long long)handle->stats.footprint_after, (unsigned long long)handle->stats.peak_footprint);
        if (handle->observer) handle->observer(&handle->stats, handle->observer_context);
        if (status) return status;
    }
    return 0;
}
