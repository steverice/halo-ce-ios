/* Exercise the exact tile pack/crop code without loading Core ML or requiring a Neural Engine. */
#include "../host/texture_upscale_model.m"
#include <assert.h>

/* A deterministic 4x predictor verifies tile stitching and cropping without inference hardware. */
@interface TilePredictor : NSObject
- (id<MLFeatureProvider>)predictionFromFeatures:(id<MLFeatureProvider>)features
    options:(MLPredictionOptions *)options error:(NSError **)error;
@end
@implementation TilePredictor
- (id<MLFeatureProvider>)predictionFromFeatures:(id<MLFeatureProvider>)features
    options:(MLPredictionOptions *)options error:(NSError **)error
{
    MLMultiArray *source = [features featureValueForName:@"image"].multiArrayValue;
    MLMultiArray *dest = options.outputBackings[@"upscaled"];
    const float *input = source.dataPointer;
    float *output = dest.dataPointer;
    unsigned side = MODEL_SIDE * MODEL_SCALE;
    for (unsigned c = 0; c < 3; c++) for (unsigned y = 0; y < side; y++) for (unsigned x = 0; x < side; x++)
        output[(size_t)c * side * side + y * side + x] = input[(size_t)c * MODEL_SIDE * MODEL_SIDE +
            (y / MODEL_SCALE) * MODEL_SIDE + x / MODEL_SCALE];
    return [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{@"upscaled": dest} error:error];
}
@end

int main(void)
{
    @autoreleasepool {
        float input[5 * 3 * 3];
        for (unsigned y = 0; y < 3; y++) for (unsigned x = 0; x < 5; x++)
            for (unsigned c = 0; c < 3; c++) input[(y * 5 + x) * 3 + c] = y * 100 + x * 10 + c;
        float *tile = calloc(3 * MODEL_SIDE * MODEL_SIDE, sizeof(float));
        assert(tile);
        for (unsigned c = 0; c < 3; c++) pack_tile(tile + c * MODEL_SIDE * MODEL_SIDE, input, 5, 3, 3, c, 0, 0, 5, 3, 1);
        for (unsigned y = 0; y < MODEL_SIDE; y++) for (unsigned x = 0; x < MODEL_SIDE; x++) {
            int sy = ((int)(y < 35 ? y : 34) - 16) % 3;
            int sx = ((int)(x < 37 ? x : 36) - 16) % 5;
            if (sy < 0) sy += 3;
            if (sx < 0) sx += 5;
            for (unsigned c = 0; c < 3; c++)
                assert(tile[c * MODEL_SIDE * MODEL_SIDE + y * MODEL_SIDE + x] == input[(sy * 5 + sx) * 3 + c]);
        }
        pack_tile(tile, input, 5, 3, 3, 0, 0, 0, 5, 3, 0);
        assert(tile[0] == input[0]);
        assert(tile[MODEL_SIDE * MODEL_SIDE - 1] == input[(2 * 5 + 4) * 3]);
        /* An interior tile sees its actual left/top neighbors, including at the next tile boundary. */
        float *large = calloc(600 * 2, sizeof(float));
        assert(large);
        for (unsigned i = 0; i < 1200; i++) large[i] = i;
        pack_tile(tile, large, 600, 2, 1, 0, 256, 0, 256, 2, 0);
        assert(tile[16 * MODEL_SIDE] == 240);
        assert(tile[16 * MODEL_SIDE + 16] == 256);
        assert(tile[16 * MODEL_SIDE + MODEL_SIDE - 1] == 527);
        /* One channel fills only its own slot: three single-channel tiles share a call. */
        tile[MODEL_SIDE * MODEL_SIDE + 16 * MODEL_SIDE + 16] = -1;
        pack_tile(tile, large, 600, 2, 1, 0, 256, 0, 256, 2, 0);
        assert(tile[MODEL_SIDE * MODEL_SIDE + 16 * MODEL_SIDE + 16] == -1);
        float bad_out[16];
        char error[128];
        assert(texture_upscale_model_predict4x(NULL, input, 5, 3, 3, 0, bad_out, error, sizeof(error)) == -1);
        assert(error[0]);
        TextureUpscaleModel *handle = [TextureUpscaleModel new];
        handle->model = (MLModel *)(id)[TilePredictor new];
        handle->planes = 3;
        for (int i = 0; i < 2; i++) {
            handle->input[i] = make_array(3, MODEL_SIDE, MLMultiArrayDataTypeFloat32, NULL);
            handle->backing[i] = make_array(3, MODEL_SIDE * MODEL_SCALE, MLMultiArrayDataTypeFloat32, NULL);
            handle->provider[i] = [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{@"image":handle->input[i]} error:NULL];
            handle->options[i] = [MLPredictionOptions new];
            handle->options[i].outputBackings = @{@"upscaled": handle->backing[i]};
            assert(handle->input[i] && handle->backing[i] && handle->provider[i]);
        }
        handle->consumers = dispatch_queue_create("probe", DISPATCH_QUEUE_SERIAL);
        float *stitched = calloc(600 * 2 * 16 * 4, sizeof(float));
        assert(stitched);
        assert(texture_upscale_model_predict4x((__bridge struct texture_upscale_model *)handle, large,
            600, 2, 1, 1, stitched, error, sizeof(error)) == 0);
        for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 2400; x++)
            assert(stitched[y * 2400 + x] == large[(y / 4) * 600 + x / 4]);
        assert(handle->stats.predictions == 1 && handle->stats.tiles == 3);
        for (unsigned i = 0; i < 600 * 2 * 16 * 4; i++) stitched[i] = -777;
        assert(texture_upscale_model_predict4x_strided((__bridge struct texture_upscale_model *)handle, large,
            600, 2, 1, 1, stitched + 2, 4, error, sizeof(error)) == 0);
        for (unsigned y = 0; y < 8; y++) for (unsigned x = 0; x < 2400; x++) {
            size_t i = ((size_t)y * 2400 + x) * 4;
            assert(stitched[i + 2] == large[(y / 4) * 600 + x / 4]);
            assert(stitched[i] == -777 && stitched[i + 1] == -777 && stitched[i + 3] == -777);
        }
        float rgb_out[5 * 3 * 16 * 3];
        assert(texture_upscale_model_predict4x((__bridge struct texture_upscale_model *)handle, input,
            5, 3, 3, 0, rgb_out, error, sizeof(error)) == 0);
        for (unsigned y = 0; y < 12; y++) for (unsigned x = 0; x < 20; x++) for (unsigned c = 0; c < 3; c++)
            assert(rgb_out[(y * 20 + x) * 3 + c] == input[((y / 4) * 5 + x / 4) * 3 + c]);
        free(stitched);
        free(large);
        free(tile);
        puts("texture upscale model tile probe passed");
    }
    return 0;
}
